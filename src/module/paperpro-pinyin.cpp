// PaperPro Pinyin: Chinese pinyin input for the reMarkable Paper Pro's own UI
// (xochitl), on the on-screen keyboard and on the Type Folio alike.
//
// Loaded into xochitl with LD_PRELOAD. It patches nothing in xochitl: an
// application-wide event filter sees
//   - the Type Folio's key presses, on their way to the window;
//   - what xochitl's on-screen keyboard types, which it delivers as
//     QInputMethodEvent commits to the focused editor;
// letters go to Rime (the pinyin-rime helper, rime-ice schema, run as a child
// process), and the chosen words reach the editor as a QInputMethodEvent
// commit, exactly as the on-screen keyboard's own text does, so every editor
// that takes typing (notebook text included) takes them. The candidate bar is
// a QML item placed into xochitl's own scene, below the text cursor.
//
// Files live next to the library (its directory is found at run time):
//   libpaperpro-pinyin.so, rime/ (engine + dictionaries), user/ (learnt words),
//   xochitl-starts (crash guard), and the switches disabled / debug.
//
// Safety: it acts only in xochitl itself; it stands down for the rest of the
// boot after three starts that did not last a minute (so a crash cannot loop
// the tablet), and when the file "disabled" exists next to it.
#include <QtCore>
#include <QtGui>
#include <QtQml>
#include <QtQuick>

#include <atomic>
#include <cstdio>
#include <dlfcn.h>
#include <fcntl.h>
#include <csignal>
#include <sys/stat.h>
#include <unistd.h>

// The directory this library was loaded from.
static QString baseDir() {
    static const QString dir = [] {
        Dl_info info;
        if (dladdr(reinterpret_cast<void *>(&baseDir), &info) && info.dli_fname)
            return QFileInfo(QString::fromLocal8Bit(info.dli_fname)).absolutePath();
        return QStringLiteral("/home/root/paperpro-pinyin");
    }();
    return dir;
}
static QString basePath(const char *name) { return baseDir() + QLatin1Char('/') + QLatin1String(name); }

static void logf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("[paperpro-pinyin] ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

#ifndef PINYIN_VERSION
#define PINYIN_VERSION "dev"
#endif

#include "bluetooth.h"
#include "voice.h"

// The candidate bar, the editing toolbar and the settings page (overlay.qml,
// turned into a string constant at build time).
#include "overlay.qml.h"

class PinyinIme : public QObject {
    Q_OBJECT
public:
    PinyinIme() {
        connect(qApp, &QCoreApplication::aboutToQuit, this, [this] {
            setGuard(0);
            if (m_proc) m_proc->disconnect(this);  // its exit now is expected
        });
        if (m_debug) openControl();
        QTimer::singleShot(5000, this, [this] { launchRime(); });
        m_bt = new Bluetooth(this);
        m_voice = new Voice(this);
    }

    Q_INVOKABLE void pick(int i) { rime(QStringLiteral("pick %1").arg(i)); }
    Q_INVOKABLE void page(int d) { rime(QStringLiteral("page %1").arg(d > 0 ? 1 : 0)); }
    // 设置页打开时输入法让开：按键照常交给设置页（Esc 关闭它）。
    Q_INVOKABLE void settingsShown(bool open) {
        m_settingsOpen = open;
        if (open && !m_preedit.isEmpty()) rime(QStringLiteral("clear"));
    }
    void showSettings(bool open) {
        if (m_bar) m_bar->setProperty("settingsOpen", open);
    }
    // 工具条上的「语音」。录音还没接上：先去设置页配好麦克风。
    Q_INVOKABLE void voice() {
        logf("voice: requested (mic %s)", qPrintable(m_bt->mic()));
        showSettings(true);
    }
    Q_INVOKABLE void toggle() {
        rime(QStringLiteral("ascii %1").arg(m_ascii ? 0 : 1));
        logf("mode: %s", m_ascii ? "English" : "Chinese");
    }

    // Starts that did not last a minute, counted per boot ("<boot id> <n>"):
    // a fresh boot always tries again, and a crash cannot loop one boot into
    // xochitl's own start limit (4).
    static QByteArray bootId() {
        QFile f(QStringLiteral("/proc/sys/kernel/random/boot_id"));
        return f.open(QIODevice::ReadOnly) ? f.readAll().trimmed() : QByteArray();
    }
    static int guard() {
        QFile f(basePath("xochitl-starts"));
        if (!f.open(QIODevice::ReadOnly)) return 0;
        QList<QByteArray> v = f.readAll().trimmed().split(' ');
        return v.size() == 2 && v[0] == bootId() ? v[1].toInt() : 0;
    }
    static void setGuard(int n) {
        QFile f(basePath("xochitl-starts"));
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) f.write(bootId() + ' ' + QByteArray::number(n));
    }

protected:
    bool eventFilter(QObject *o, QEvent *e) override {
        if (m_debug && (e->type() == QEvent::KeyPress || e->type() == QEvent::InputMethod)) {
            QObject *fo = QGuiApplication::focusObject();
            if (e->type() == QEvent::KeyPress) {
                auto *k = static_cast<QKeyEvent *>(e);
                logf("key 0x%x '%s' on %s%s (focus %s, editable %d)", k->key(), qPrintable(k->text()),
                     o->metaObject()->className(), e->spontaneous() ? " spontaneous" : "",
                     fo ? fo->metaObject()->className() : "-", editable(fo));
            } else {
                auto *im = static_cast<QInputMethodEvent *>(e);
                logf("commit '%s' preedit '%s' repl %d/%d on %s%s", qPrintable(im->commitString()),
                     qPrintable(im->preeditString()), im->replacementStart(), im->replacementLength(),
                     o->metaObject()->className(), o == fo ? " (focus)" : "");
            }
        }
        switch (e->type()) {
        case QEvent::KeyPress:
        case QEvent::KeyRelease:
            // Hardware keys arrive at the window first; keys an on-screen
            // keyboard sends straight to the editor are not spontaneous.
            if (o->isWindowType() || (o == QGuiApplication::focusObject() && !e->spontaneous()))
                return key(static_cast<QKeyEvent *>(e));
            break;
        case QEvent::InputMethod:
            if (!m_committing && o == QGuiApplication::focusObject())
                return inputMethod(static_cast<QInputMethodEvent *>(e));
            break;
        case QEvent::FocusIn:
        case QEvent::FocusOut:
            QTimer::singleShot(0, this, [this] { focusChanged(); });
            break;
        default:
            break;
        }
        return false;
    }

private:
    // ---- debug control: a FIFO for working on this without seeing the glass ----
    //   type <letters>   as if typed on a keyboard
    //   key <keysym>     one key to Rime
    //   dump             where the bar is and what has the focus
    //   grab             the focused window as /tmp/paperpro-pinyin-grab.png
    //   call <method>    a method of the focused item or an ancestor
    //   prop <name>      a property of the focused item or an ancestor
    //   settings [off]   open / close the settings page
    //   bt on|off|scan   Bluetooth, as the settings page does
    //   voice test       the DeepSeek connection test
    //   state            Bluetooth devices and test results
    void openControl() {
        QByteArray path = basePath("ctl").toLocal8Bit();
        unlink(path.constData());
        if (mkfifo(path.constData(), 0600) != 0) return;
        m_ctlFd = open(path.constData(), O_RDWR | O_NONBLOCK); // RDWR: never sees EOF
        if (m_ctlFd < 0) return;
        auto *n = new QSocketNotifier(m_ctlFd, QSocketNotifier::Read, this);
        connect(n, &QSocketNotifier::activated, this, [this] {
            char buf[512];
            ssize_t got = read(m_ctlFd, buf, sizeof buf - 1);
            if (got <= 0) return;
            m_ctlBuf += QByteArray(buf, int(got));
            int nl;
            while ((nl = m_ctlBuf.indexOf('\n')) >= 0) {
                control(QString::fromUtf8(m_ctlBuf.left(nl)).trimmed());
                m_ctlBuf.remove(0, nl + 1);
            }
        });
        logf("debug control at %s", path.constData());
    }

    void control(const QString &cmd) {
        if (cmd.startsWith(QLatin1String("type "))) {
            for (QChar c : cmd.mid(5)) rime(QStringLiteral("key %1 0").arg(c.unicode()));
        } else if (cmd.startsWith(QLatin1String("key "))) {
            rime(QStringLiteral("key %1 0").arg(cmd.mid(4).toInt(nullptr, 0)));
        } else if (cmd.startsWith(QLatin1String("call ")) || cmd.startsWith(QLatin1String("prop "))) {
            // call <method> / prop <name>: on the focused item or its nearest
            // ancestor that has it.
            QByteArray name = cmd.mid(5).toLatin1();
            bool call = cmd.startsWith(QLatin1String("call "));
            for (QQuickItem *a = qobject_cast<QQuickItem *>(QGuiApplication::focusObject()); a; a = a->parentItem()) {
                const QMetaObject *mo = a->metaObject();
                if (call && mo->indexOfMethod(name + "()") >= 0) {
                    QVariant ret;
                    bool ok = mo->method(mo->indexOfMethod(name + "()")).returnType() == QMetaType::QVariant
                                  ? QMetaObject::invokeMethod(a, name.constData(), Q_RETURN_ARG(QVariant, ret))
                                  : QMetaObject::invokeMethod(a, name.constData());
                    logf("call %s on %s: %d", name.constData(), mo->className(), ok);
                    return;
                }
                if (!call && mo->indexOfProperty(name.constData()) >= 0) {
                    logf("prop %s on %s = %s", name.constData(), mo->className(),
                         qPrintable(a->property(name.constData()).toString()));
                    return;
                }
            }
            logf("%s: not found", name.constData());
        } else if (cmd == QLatin1String("dump")) {
            QObject *fo = QGuiApplication::focusObject();
            auto *item = qobject_cast<QQuickItem *>(fo);
            QRectF cur;
            if (item) {
                QInputMethodQueryEvent q(Qt::ImCursorRectangle);
                QCoreApplication::sendEvent(item, &q);
                cur = q.value(Qt::ImCursorRectangle).toRectF();
            }
            logf("focus %s editable %d cursor(local) %.0f,%.0f %.0fx%.0f", fo ? fo->metaObject()->className() : "-",
                 editable(fo), cur.x(), cur.y(), cur.width(), cur.height());
            if (item && m_barRoot) {
                QRectF sc;
                qreal margin = 0;
                logf("scene cursor found %d: %.0f,%.0f %.0fx%.0f keyboard margin %.0f", sceneCursor(item, &sc, &margin),
                     sc.x(), sc.y(), sc.width(), sc.height(), margin);
            }
            for (QQuickItem *a = item; a; a = a->parentItem())
                logf("  up %s %.0f,%.0f %.0fx%.0f rot %.0f vis %d op %.2f clip %d z %.0f", a->metaObject()->className(),
                     a->x(), a->y(), a->width(), a->height(), a->rotation(), a->isVisible(), a->opacity(), a->clip(), a->z());
            if (item && item->window())
                for (QQuickItem *c : item->window()->contentItem()->childItems())
                    logf("  top %s %.0fx%.0f rot %.0f vis %d op %.2f z %.0f", c->metaObject()->className(), c->width(),
                         c->height(), c->rotation(), c->isVisible(), c->opacity(), c->z());
            for (QWindow *w : QGuiApplication::topLevelWindows())
                logf("window %s %dx%d visible %d", w->metaObject()->className(), w->width(), w->height(), w->isVisible());
            if (m_bar) {
                QQuickItem *b = m_bar->childItems().value(0);
                logf("bar root %s %.0fx%.0f in %s; bar %.0f,%.0f %.0fx%.0f visible %d; preedit '%s' cands %d",
                     m_bar->metaObject()->className(), m_bar->width(), m_bar->height(),
                     m_barRoot ? m_barRoot->metaObject()->className() : "-", b ? b->x() : -1, b ? b->y() : -1,
                     b ? b->width() : -1, b ? b->height() : -1, b ? b->isVisible() : -1, qPrintable(m_preedit),
                     int(m_cands.size()));
                QRectF c = m_bar->property("cursor").toRectF();
                logf("bar cursor (mapped) %.0f,%.0f %.0fx%.0f; root rotation %.0f scale %.2f",
                     c.x(), c.y(), c.width(), c.height(), m_barRoot ? m_barRoot->rotation() : 0,
                     m_barRoot ? m_barRoot->scale() : 0);
            } else {
                logf("no bar");
            }
        } else if (cmd == QLatin1String("settings") || cmd == QLatin1String("settings off")) {
            showSettings(cmd == QLatin1String("settings"));
        } else if (cmd == QLatin1String("bt on") || cmd == QLatin1String("bt off")) {
            m_bt->setEnabled(cmd == QLatin1String("bt on"));
        } else if (cmd == QLatin1String("bt scan")) {
            m_bt->scan(!m_bt->scanning());
        } else if (cmd == QLatin1String("voice test")) {
            m_voice->testKey();
        } else if (cmd == QLatin1String("state")) {
            logf("bt %s scanning %d mic '%s' message '%s' devices %d; voice %s '%s'", qPrintable(m_bt->state()),
                 m_bt->scanning(), qPrintable(m_bt->mic()), qPrintable(m_bt->message()), int(m_bt->devices().size()),
                 qPrintable(m_voice->testState()), qPrintable(m_voice->testMessage()));
            for (const QVariant &d : m_bt->devices()) {
                QVariantMap m = d.toMap();
                logf("  %s %s paired %d audio %d", qPrintable(m.value("address").toString()),
                     qPrintable(m.value("name").toString()), m.value("paired").toBool(), m.value("audio").toBool());
            }
        } else if (cmd.startsWith(QLatin1String("methods "))) {
            // methods <regex>: invokable methods and properties (of every class
            // reachable from the windows' items) whose names match.
            QRegularExpression re(cmd.mid(8), QRegularExpression::CaseInsensitiveOption);
            QSet<const QMetaObject *> seen;
            std::function<void(QObject *, int)> walk = [&](QObject *o, int depth) {
                if (depth > 60) return;
                for (const QMetaObject *mo = o->metaObject(); mo && mo != &QObject::staticMetaObject; mo = mo->superClass()) {
                    if (seen.contains(mo)) break;
                    seen.insert(mo);
                    for (int i = mo->methodOffset(); i < mo->methodCount(); i++) {
                        QMetaMethod m = mo->method(i);
                        if (m.methodType() == QMetaMethod::Signal) continue;
                        if (re.match(QString::fromLatin1(m.name())).hasMatch())
                            logf("  %s::%s", mo->className(), m.methodSignature().constData());
                    }
                    for (int i = mo->propertyOffset(); i < mo->propertyCount(); i++)
                        if (re.match(QString::fromLatin1(mo->property(i).name())).hasMatch())
                            logf("  %s.%s = %s", mo->className(), mo->property(i).name(),
                                 qPrintable(o->property(mo->property(i).name()).toString().left(80)));
                }
                for (QObject *c : o->children()) walk(c, depth + 1);
                if (auto *it = qobject_cast<QQuickItem *>(o))
                    for (QQuickItem *c : it->childItems()) walk(c, depth + 1);
            };
            for (QWindow *w : QGuiApplication::topLevelWindows())
                if (auto *qw = qobject_cast<QQuickWindow *>(w)) walk(qw->contentItem(), 0);
            for (QObject *c : qApp->children()) walk(c, 0);
            logf("methods: done");
        } else if (cmd == QLatin1String("rt")) {
            // Render timing: how long each frame of the bar's window takes.
            auto *w = m_barWindow.data();
            if (!w) { logf("rt: no window"); return; }
            static QElapsedTimer t;
            static bool hooked = false;
            if (!hooked) {
                hooked = true;
                logf("rt: render loop thread %s", w->thread() == QThread::currentThread() ? "gui" : "other");
                connect(w, &QQuickWindow::beforeSynchronizing, this, [] { t.start(); }, Qt::DirectConnection);
                connect(w, &QQuickWindow::afterRendering, this, [] {
                    if (t.isValid()) logf("rt: frame %lld ms", t.elapsed());
                }, Qt::DirectConnection);
            }
        } else if (cmd.startsWith(QLatin1String("vis "))) {
            // vis <name> 0|1: hide or show one part of the overlay (bar, tools, page, chip, all)
            QStringList a = cmd.split(QLatin1Char(' '));
            if (a.size() == 3 && m_bar) {
                if (a[1] == QLatin1String("all")) m_bar->setVisible(a[2] == QLatin1String("1"));
                else if (a[1] == QLatin1String("root"))
                    m_bar->setSize(a[2] == QLatin1String("1") && m_barRoot ? m_barRoot->size() : QSizeF(0, 0));
                logf("vis %s %s: bar visible %d size %.0fx%.0f", qPrintable(a[1]), qPrintable(a[2]), m_bar->isVisible(),
                     m_bar->width(), m_bar->height());
            }
        } else if (cmd.startsWith(QLatin1String("probe "))) {
            // probe <qml>: create it in xochitl's engine and list its class's
            // properties and enums (e.g. probe import xofm.libs.epaper; ScreenModeItem {})
            QQmlEngine *engine = m_bar ? qmlEngine(m_bar) : nullptr;
            if (!engine) { logf("probe: no engine yet"); return; }
            auto *c = new QQmlComponent(engine, this);
            QString src = cmd.mid(6);
            src.replace(QLatin1String("; "), QLatin1String("\n"));
            c->setData(src.toUtf8(), QUrl(QStringLiteral("paperpro-pinyin:/probe.qml")));
            auto done = [this, c] {
                if (c->isLoading()) return;
                QObject *o = c->isReady() ? c->create() : nullptr;
                if (!o) { logf("probe: %s", qPrintable(c->errorString())); c->deleteLater(); return; }
                for (const QMetaObject *mo = o->metaObject(); mo; mo = mo->superClass()) {
                    logf("probe class %s", mo->className());
                    if (QByteArray(mo->className()) == "QQuickItem" || QByteArray(mo->className()) == "QObject") break;
                    for (int i = mo->enumeratorOffset(); i < mo->enumeratorCount(); i++) {
                        QMetaEnum e = mo->enumerator(i);
                        QStringList keys;
                        for (int k = 0; k < e.keyCount(); k++) keys << QStringLiteral("%1=%2").arg(e.key(k)).arg(e.value(k));
                        logf("  enum %s: %s", e.name(), qPrintable(keys.join(QLatin1String(", "))));
                    }
                    for (int i = mo->propertyOffset(); i < mo->propertyCount(); i++)
                        logf("  prop %s %s = %s", mo->property(i).typeName(), mo->property(i).name(),
                             qPrintable(o->property(mo->property(i).name()).toString()));
                    for (int i = mo->methodOffset(); i < mo->methodCount(); i++)
                        logf("  method %s", mo->method(i).methodSignature().constData());
                }
                delete o;
                c->deleteLater();
            };
            connect(c, &QQmlComponent::statusChanged, this, done);
            done();
        } else if (cmd.startsWith(QLatin1String("fastmode "))) {
            if (m_bar) m_bar->setProperty("fastMode", cmd.mid(9).toInt());
            logf("fastmode %s", qPrintable(m_bar ? m_bar->property("fastMode").toString() : QStringLiteral("-")));
        } else if (cmd == QLatin1String("modes")) {
            // Every e-paper screen-mode region in the UI: its mode, size and owners.
            std::function<void(QQuickItem *)> walk = [&](QQuickItem *it) {
                if (it->inherits("EPScreenModeItem")) {
                    QStringList up;
                    for (QQuickItem *a = it->parentItem(); a && up.size() < 4; a = a->parentItem())
                        up << QString::fromLatin1(a->metaObject()->className()).section(QLatin1Char('_'), 0, 0);
                    QRectF r = it->mapRectToScene(it->boundingRect());
                    logf("  mode %s vis %d %.0f,%.0f %.0fx%.0f in %s", qPrintable(it->property("mode").toString()),
                         it->isVisible(), r.x(), r.y(), r.width(), r.height(), qPrintable(up.join(QLatin1String(" < "))));
                }
                for (QQuickItem *c : it->childItems()) walk(c);
            };
            for (QWindow *w : QGuiApplication::topLevelWindows())
                if (auto *qw = qobject_cast<QQuickWindow *>(w)) walk(qw->contentItem());
            logf("modes: done");
        } else if (cmd == QLatin1String("grab")) {
            auto *w = qobject_cast<QQuickWindow *>(m_barWindow ? m_barWindow.data() : QGuiApplication::focusWindow());
            if (!w) { logf("grab: no window"); return; }
            QImage img = w->grabWindow();
            logf("grab: %dx%d saved %d", img.width(), img.height(), img.save(QStringLiteral("/tmp/paperpro-pinyin-grab.png")));
        }
    }

    int m_ctlFd = -1;
    QByteArray m_ctlBuf;

    // ---- the focused editor ------------------------------------------------------
    static bool editable(QObject *fo) {
        if (!fo) return false;
        QInputMethodQueryEvent q(Qt::ImEnabled);
        QCoreApplication::sendEvent(fo, &q);
        return q.value(Qt::ImEnabled).toBool();
    }

    void focusChanged() {
        bool ed = editable(QGuiApplication::focusObject());
        if (!ed && !m_preedit.isEmpty()) rime(QStringLiteral("clear"));
        m_editing = ed;
        updateBar();
    }

    void commit(const QString &text) {
        QObject *fo = QGuiApplication::focusObject();
        if (!fo || text.isEmpty()) return;
        QInputMethodEvent ev;
        ev.setCommitString(text);
        m_committing = true;
        QCoreApplication::sendEvent(fo, &ev);
        m_committing = false;
    }

    // ---- keys -------------------------------------------------------------------------
    bool key(QKeyEvent *k) {
        Qt::KeyboardModifiers mods = k->modifiers();
        if (m_settingsOpen) {
            if (k->type() == QEvent::KeyPress && k->key() == Qt::Key_Escape) {
                showSettings(false);
                return true;
            }
            return false;
        }
        // A tap on Shift alone switches 中/英, as in common input methods.
        if (k->key() == Qt::Key_Shift) {
            if (k->type() == QEvent::KeyPress && !k->isAutoRepeat()) m_shiftAlone = true;
            else if (k->type() == QEvent::KeyRelease && m_shiftAlone && editable(QGuiApplication::focusObject())) {
                m_shiftAlone = false;
                toggle();
            }
            return false;
        }
        if (k->type() == QEvent::KeyRelease) return m_swallowRelease.remove(k->key());
        m_shiftAlone = false;
        if (!editable(QGuiApplication::focusObject())) return false;
        if (k->key() == Qt::Key_Space && (mods & Qt::ControlModifier)) {
            toggle();
            return true;
        }
        if (mods & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) return false;
        int sym = keysym(k);
        if (!sym) return false;
        if (rime(QStringLiteral("key %1 %2").arg(sym).arg(mods & Qt::ShiftModifier ? 1 : 0))) {
            m_swallowRelease.insert(k->key());
            return true;
        }
        return false;
    }

    static int keysym(QKeyEvent *k) {
        switch (k->key()) {
        case Qt::Key_Backspace: return 0xff08;
        case Qt::Key_Tab: return 0xff09;
        case Qt::Key_Return:
        case Qt::Key_Enter: return 0xff0d;
        case Qt::Key_Escape: return 0xff1b;
        case Qt::Key_Delete: return 0xffff;
        case Qt::Key_Home: return 0xff50;
        case Qt::Key_Left: return 0xff51;
        case Qt::Key_Up: return 0xff52;
        case Qt::Key_Right: return 0xff53;
        case Qt::Key_Down: return 0xff54;
        case Qt::Key_PageUp: return 0xff55;
        case Qt::Key_PageDown: return 0xff56;
        case Qt::Key_End: return 0xff57;
        }
        QString t = k->text();
        if (t.size() == 1 && t[0].unicode() >= 0x20 && t[0].unicode() <= 0x7e) return t[0].unicode();
        return 0;
    }

    // What the on-screen keyboard types: a letter, a space, a backspace (an
    // empty commit replacing the character before the cursor), sent to the
    // editor as input method events.
    bool inputMethod(QInputMethodEvent *im) {
        if (!editable(QGuiApplication::focusObject()) || !im->preeditString().isEmpty()) return false;
        const QString c = im->commitString();
        bool composing = !m_preedit.isEmpty();
        if (c.isEmpty() && im->replacementLength() > 0 && im->replacementStart() < 0 && composing)
            return rime(QStringLiteral("key %1 0").arg(0xff08));
        if (c.size() != 1) {
            if (c == QLatin1String("\n") && composing) return rime(QStringLiteral("key %1 0").arg(0xff0d));
            return false;
        }
        ushort ch = c[0].unicode();
        if (ch == '\n' || ch == '\r') return composing && rime(QStringLiteral("key %1 0").arg(0xff0d));
        if (ch < 0x20 || ch > 0x7e) return false;
        bool upper = ch >= 'A' && ch <= 'Z';
        return rime(QStringLiteral("key %1 %2").arg(ch).arg(upper ? 1 : 0));
    }

    // ---- Rime --------------------------------------------------------------------------
    // Rime is started in the background soon after xochitl comes up: its first
    // start after a reboot reads ~60 MB of tables and can take several
    // seconds, which should not land on the first key typed.
    void launchRime() {
        if (m_proc && m_proc->state() != QProcess::NotRunning) return;
        if (m_retry.isValid() && m_retry.elapsed() < 10000) return;
        m_retry.invalidate();
        if (m_proc) m_proc->deleteLater();
        m_proc = new QProcess(this);
        m_ready = false;
        QString rimeDir = basePath("rime");
        QDir().mkpath(basePath("user"));
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.remove(QStringLiteral("LD_PRELOAD"));
        env.insert(QStringLiteral("LD_LIBRARY_PATH"), rimeDir + QStringLiteral("/lib"));
        m_proc->setProcessEnvironment(env);
        m_proc->setProcessChannelMode(QProcess::ForwardedErrorChannel);
        QProcess *p = m_proc;
        connect(p, &QProcess::finished, this, [this, p](int code, QProcess::ExitStatus st) {
            if (p != m_proc) return;
            // SIGTERM: systemd stopping xochitl's whole service, nothing wrong.
            if (!(st == QProcess::CrashExit && code == SIGTERM))
                logf("rime exited (%s %d)", st == QProcess::CrashExit ? "crash" : "code", code);
            rimeFailed();
        });
        m_launched.start();
        p->start(rimeDir + QStringLiteral("/pinyin-rime"),
                 {rimeDir + QStringLiteral("/shared"), basePath("user"),
                  rimeDir + QStringLiteral("/plugins/librime-lua.so")});
    }

    // A failed or stuck Rime is not given up on: it is retried 10 s later.
    // Meanwhile keys go through unchanged.
    void rimeFailed() {
        if (m_proc) {
            QProcess *p = m_proc;
            m_proc = nullptr;
            p->disconnect(this);
            p->kill();
            p->deleteLater();
        }
        m_ready = false;
        m_retry.start();
        m_preedit.clear();
        m_cands.clear();
        updateBar();
    }

    bool startRime() {
        if (m_proc && m_ready && m_proc->state() == QProcess::Running) return true;
        launchRime();
        if (!m_proc) return false;
        // Its first reply means the engine is ready. Allow a cold start its
        // time (counted from launch), once.
        int left = 20000 - int(m_launched.elapsed());
        if ((m_proc->state() == QProcess::Starting && !m_proc->waitForStarted(qMax(left, 1)))
            || !readReply(qMax(left, 1))) {
            logf("rime did not start within 20 s");
            rimeFailed();
            return false;
        }
        m_ready = true;
        logf("rime ready");
        return true;
    }

    bool readReply(int timeout = 3000) {
        QElapsedTimer t;
        t.start();
        QProcess *p = m_proc;
        // (A wait can report the helper's exit, which clears m_proc.)
        while (p == m_proc && !p->canReadLine()) {
            if (t.elapsed() >= timeout || !p->waitForReadyRead(timeout - int(t.elapsed()))) break;
        }
        if (!m_proc || p != m_proc || !p->canReadLine()) return false;
        QJsonObject r = QJsonDocument::fromJson(p->readLine()).object();
        m_handled = r.value(QLatin1String("handled")).toBool();
        m_preedit = r.value(QLatin1String("preedit")).toString();
        m_ascii = r.value(QLatin1String("ascii")).toBool();
        m_cands.clear();
        for (const QJsonValue &v : r.value(QLatin1String("cands")).toArray()) m_cands << v.toString();
        m_prev = r.value(QLatin1String("page")).toInt() > 0;
        m_next = !r.value(QLatin1String("last")).toBool();
        QString c = r.value(QLatin1String("commit")).toString();
        if (!c.isEmpty()) commit(c);
        return true;
    }

    // Sends a request; true when Rime took the key.
    bool rime(const QString &req) {
        if (!startRime()) return false;
        m_proc->write(req.toUtf8() + '\n');
        if (!readReply()) {
            logf("rime stopped answering");
            rimeFailed();
            return false;
        }
        updateBar();
        return m_handled;
    }

    // ---- the bar ------------------------------------------------------------------------
    // Notebook text is drawn by xochitl's scene, which does not report its
    // cursor to input methods; its controller knows it, in page coordinates,
    // and the scene view (DeviceSceneView) maps those to the screen.
    bool sceneCursor(QQuickItem *item, QRectF *cursor, qreal *margin) {
        for (QQuickItem *a = item; a; a = a->parentItem()) {
            if (a->metaObject()->indexOfMethod("sceneToView(QVariant)") < 0) continue;
            QObject *ctl = a->property("controller").value<QObject *>();
            if (!ctl) return false;
            QVariant pos = ctl->property("textCursorPosition");
            if (!pos.isValid()) return false;
            QPointF p = pos.toPointF();
            qreal lh = ctl->property("textLineHeight").toReal();
            if (lh <= 0) lh = 32;
            // textCursorPosition is on the text's baseline.
            QVariant top, bottom;
            if (!QMetaObject::invokeMethod(a, "sceneToView", Q_RETURN_ARG(QVariant, top),
                                           Q_ARG(QVariant, QVariant(QPointF(p.x(), p.y() - 0.8 * lh)))) ||
                !QMetaObject::invokeMethod(a, "sceneToView", Q_RETURN_ARG(QVariant, bottom),
                                           Q_ARG(QVariant, QVariant(QPointF(p.x(), p.y() + 0.3 * lh)))))
                return false;
            QPointF t = jsPoint(top), b = jsPoint(bottom);
            if (m_debug)
                logf("scene cursor %.0f,%.0f lh %.0f -> view %.0f,%.0f..%.0f (%s)", p.x(), p.y(), lh, t.x(), t.y(), b.y(),
                     top.typeName());
            if (b.y() <= t.y()) return false;
            *cursor = a->mapRectToItem(m_barRoot, QRectF(t.x(), t.y(), 2, b.y() - t.y()));
            *margin = a->property("keyboardMargin").toReal();
            return true;
        }
        return false;
    }

    // A point back from QML: a QPointF, or a JS object with x and y.
    static QPointF jsPoint(const QVariant &v) {
        if (v.canConvert<QJSValue>()) {
            QJSValue j = v.value<QJSValue>();
            if (j.isObject()) return QPointF(j.property(QStringLiteral("x")).toNumber(), j.property(QStringLiteral("y")).toNumber());
        }
        if (v.typeId() == QMetaType::QVariantMap) {
            QVariantMap m = v.toMap();
            return QPointF(m.value(QStringLiteral("x")).toReal(), m.value(QStringLiteral("y")).toReal());
        }
        return v.toPointF();
    }

    // The bar goes into the top-level layer that holds the editor: xochitl
    // stacks several full-screen layers (shortcuts, gestures, the main UI)
    // under the window, and only the editor's own is surely on top. It is
    // rotated with the screen, so the bar is too.
    static QQuickItem *rootItem(QQuickItem *item) {
        QQuickItem *content = item && item->window() ? item->window()->contentItem() : nullptr;
        for (QQuickItem *a = item; a; a = a->parentItem())
            if (a->parentItem() == content) return a;
        return nullptr;
    }

    void updateBar() { pushBar(); }

    QByteArray overlayQml() const {
        if (!m_plainOverlay) return QByteArray(kOverlayQml);
        QList<QByteArray> out;
        for (const QByteArray &l : QByteArray(kOverlayQml).split('\n')) {
            if (l.contains("// FAST-REGION")) {
                if (l.contains("component FastRegion")) out << "    component FastRegion: Item { property int mode }";
                continue;
            }
            out << l;
        }
        return out.join('\n');
    }

    void pushBar() {
        QObject *fo = QGuiApplication::focusObject();
        auto *item = qobject_cast<QQuickItem *>(fo);
        QQuickItem *root = rootItem(item);
        if (m_bar && root && root != m_barRoot && qmlEngine(root) == qmlEngine(m_bar)) {
            m_bar->setParentItem(root);
            m_barRoot = root;
            m_barWindow = root->window();
            if (m_debug) logf("candidate bar moved to %s", root->metaObject()->className());
        }
        if (root && !m_bar && !m_pending) {
            QQmlEngine *engine = qmlEngine(root);
            if (engine) {
                // xochitl's engine compiles components asynchronously: wait
                // for this one to be ready, then make the bar.
                m_pending = new QQmlComponent(engine, this);
                QPointer<QQuickItem> r = root;
                auto make = [this, r, engine] {
                    QQmlComponent *comp = m_pending;
                    if (!comp || comp->isLoading()) return;
                    m_pending = nullptr;
                    comp->deleteLater();
                    if (r && comp->isError() && !m_plainOverlay) {
                        // No e-paper screen modes on this firmware: the same
                        // overlay with plain Items in their place.
                        logf("candidate bar: no fast refresh regions (%s)", qPrintable(comp->errorString().trimmed()));
                        m_plainOverlay = true;
                        QTimer::singleShot(0, this, [this] { pushBar(); });
                        return;
                    }
                    if (!r || !comp->isReady()) {
                        logf("candidate bar: %s", qPrintable(comp->errorString()));
                        return;
                    }
                    auto *ctx = new QQmlContext(engine->rootContext(), this);
                    ctx->setContextProperty(QStringLiteral("ime"), this);
                    ctx->setContextProperty(QStringLiteral("bt"), m_bt);
                    ctx->setContextProperty(QStringLiteral("voice"), m_voice);
                    m_bar = qobject_cast<QQuickItem *>(comp->create(ctx));
                    if (!m_bar) {
                        logf("candidate bar: create failed: %s", qPrintable(comp->errorString()));
                        return;
                    }
                    m_bar->setParentItem(r);
                    m_barWindow = r->window();
                    m_barRoot = r;
                    logf("candidate bar placed in %s", r->metaObject()->className());
                    pushBar();
                };
                connect(m_pending, &QQmlComponent::statusChanged, this, make);
                m_pending->setData(overlayQml(), QUrl(QStringLiteral("paperpro-pinyin:/overlay.qml")));
                make();
            }
        }
        if (!m_bar) return;
        QRectF cursor;
        qreal margin = 0;
        if (item && m_barRoot) {
            QInputMethodQueryEvent q(Qt::ImCursorRectangle);
            QCoreApplication::sendEvent(item, &q);
            QRectF r = q.value(Qt::ImCursorRectangle).toRectF();
            if (r.height() > 0 && qAbs(r.x()) < 1e6 && qAbs(r.y()) < 1e6) cursor = item->mapRectToItem(m_barRoot, r);
            else sceneCursor(item, &cursor, &margin);
        }
        m_bar->setProperty("cursor", cursor);
        m_bar->setProperty("bottomMargin", margin);
        m_bar->setProperty("preedit", m_preedit);
        m_bar->setProperty("cands", m_cands);
        m_bar->setProperty("chinese", !m_ascii);
        m_bar->setProperty("editing", m_editing);
        m_bar->setProperty("hasPrev", m_prev);
        m_bar->setProperty("hasNext", m_next);
    }

    const bool m_debug = QFile::exists(basePath("debug"));
    QProcess *m_proc = nullptr;
    bool m_ready = false;
    QElapsedTimer m_launched, m_retry;
    bool m_handled = false, m_ascii = false, m_prev = false, m_next = false;
    bool m_committing = false, m_shiftAlone = false, m_editing = false, m_settingsOpen = false, m_plainOverlay = false;
    Bluetooth *m_bt = nullptr;
    Voice *m_voice = nullptr;
    QString m_preedit;
    QStringList m_cands;
    QSet<int> m_swallowRelease;
    QPointer<QQuickItem> m_bar;
    QPointer<QQmlComponent> m_pending;
    QPointer<QQuickWindow> m_barWindow;
    QPointer<QQuickItem> m_barRoot;
};

// ---- the on-screen keyboard's layout ---------------------------------------------
// xochitl's keyboard loads :/misc/keyboards/<Keyboard setting>/. It has no
// zh_CN layout, yet that setting is what Chinese users end up with (left by
// earlier Chinese keyboard add-ons): the keyboard then comes up empty. When
// zh_CN is missing, xochitl's own en_US layout is offered under that name too
// (a resource tree built here, in memory), and the letters typed on it become
// pinyin as Type Folio keys do.
static void putBE(QByteArray &b, quint32 v, int bytes) {
    for (int i = bytes - 1; i >= 0; i--) b.append(char((v >> (8 * i)) & 0xff));
}

static void aliasKeyboardLayout(const QString &to, const QString &from) {
    const QString base = QStringLiteral(":/misc/keyboards/");
    if (QFile::exists(base + to + QStringLiteral("/keyboard_layout.json"))) return;
    QStringList files = QDir(base + from).entryList(QDir::Files);
    if (files.isEmpty()) {
        logf("keyboard: no %s layout to offer as %s", qPrintable(from), qPrintable(to));
        return;
    }
    // Qt's resource format, version 1: header, tree, data, names. A directory's
    // children are sorted by name hash.
    std::sort(files.begin(), files.end(), [](const QString &a, const QString &b) { return qt_hash(a) < qt_hash(b); });
    struct Node { QString name; bool dir; quint32 count, child; QByteArray data; };
    QList<Node> nodes = {{QString(), true, 1, 1, {}},
                         {QStringLiteral("misc"), true, 1, 2, {}},
                         {QStringLiteral("keyboards"), true, 1, 3, {}},
                         {to, true, quint32(files.size()), 4, {}}};
    for (const QString &f : files) {
        QFile in(base + from + QLatin1Char('/') + f);
        if (!in.open(QIODevice::ReadOnly)) return;
        nodes.append({f, false, 0, 0, in.readAll()});
    }
    QByteArray tree, data, names;
    for (const Node &n : nodes) {
        quint32 nameOff = quint32(names.size());
        putBE(names, quint32(n.name.size()), 2);
        putBE(names, qt_hash(n.name), 4);
        for (QChar c : n.name) putBE(names, c.unicode(), 2);
        putBE(tree, nameOff, 4);
        putBE(tree, n.dir ? 0x02 : 0x00, 2);
        if (n.dir) {
            putBE(tree, n.count, 4);
            putBE(tree, n.child, 4);
        } else {
            putBE(tree, 0, 2); // any territory
            putBE(tree, 1, 2); // QLocale::C: any language
            putBE(tree, quint32(data.size()), 4);
            putBE(data, quint32(n.data.size()), 4);
            data += n.data;
        }
    }
    static QByteArray rcc; // must outlive its registration
    rcc = QByteArray("qres");
    const quint32 header = 20;
    putBE(rcc, 1, 4);
    putBE(rcc, header, 4);
    putBE(rcc, header + quint32(tree.size()), 4);
    putBE(rcc, header + quint32(tree.size() + data.size()), 4);
    rcc += tree + data + names;
    bool ok = QResource::registerResource(reinterpret_cast<const uchar *>(rcc.constData()));
    logf("keyboard: %s layout offered as %s: %s", qPrintable(from), qPrintable(to),
         ok && QFile::exists(base + to + QStringLiteral("/keyboard_layout.json")) ? "ok" : "failed");
}

// Runs as the QCoreApplication is constructed (the loader opens this library
// before main runs), or, when /home came up after xochitl had started, through
// paperpro_pinyin_late_start below. Either way it runs once.
static void start() {
    static std::atomic<bool> started{false};
    if (started.exchange(true)) return;
    if (!QCoreApplication::applicationFilePath().endsWith(QLatin1String("/xochitl"))) return;
    if (QFile::exists(basePath("disabled"))) {
        logf("disabled by %s", qPrintable(basePath("disabled")));
        return;
    }
    int starts = PinyinIme::guard();
    if (starts >= 3) {
        logf("stood down: xochitl ended early %d times in a row with it loaded "
             "this boot (it tries again after a restart, or remove %s)", starts, qPrintable(basePath("xochitl-starts")));
        return;
    }
    PinyinIme::setGuard(starts + 1);
    aliasKeyboardLayout(QStringLiteral("zh_CN"), QStringLiteral("en_US"));
    // A minute of running is a good start; the count goes back to zero.
    QTimer::singleShot(60000, qApp, [] { PinyinIme::setGuard(0); });
    // The application object is still being built here; the filter goes in
    // once its event loop runs.
    QTimer::singleShot(0, qApp, [] {
        auto *ime = new PinyinIme;
        ime->setParent(qApp);
        qApp->installEventFilter(ime);
        logf("loaded (version %s)", PINYIN_VERSION);
    });
}
Q_COREAPP_STARTUP_FUNCTION(start)

// Called by the loader (on its own thread) after it opened this library late.
// If the application object already existed by then, Qt has either run start()
// right away or missed it; in both cases it is run (again: a no-op) on the main
// thread. Before the application exists, Qt will run it as it is constructed.
extern "C" __attribute__((visibility("default"))) void paperpro_pinyin_late_start() {
    if (QCoreApplication *app = QCoreApplication::instance())
        QMetaObject::invokeMethod(app, [] { start(); }, Qt::QueuedConnection);
}

#include "paperpro-pinyin.moc"
#include "moc_bluetooth.cpp"
#include "moc_voice.cpp"
