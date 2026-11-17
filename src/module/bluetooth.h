// PaperPro 拼音：蓝牙（给语音输入用的麦克风）。
//
// reMarkable 没有在系统里启用蓝牙，但芯片（NXP IW61x）、驱动与固件都在。
// bluetooth-up.sh 载入驱动并启动系统自带的 bluetoothd；之后的事（开关、搜索、
// 配对、忘记）都通过 D-Bus 交给 bluetoothd，这里只是设置页背后的模型。
//
// 包含它的文件需先定义 logf() 与 basePath()。
#pragma once
#include <QtCore>
#include <QtDBus>

namespace bt {
inline const QString kService = QStringLiteral("org.bluez");
inline const QString kAdapter = QStringLiteral("/org/bluez/hci0");
inline const QString kAgentPath = QStringLiteral("/paperpro_pinyin/agent");
inline const QString kProps = QStringLiteral("org.freedesktop.DBus.Properties");
}

// 配对代理：耳机和麦克风没有屏幕也没有键盘，配对时不需要比对或输入数字
// （NoInputNoOutput）。只在设置页打开、用户自己点了“配对”时才会用到。
class BtAgent : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.bluez.Agent1")
public:
    using QObject::QObject;
public Q_SLOTS:
    void Release() {}
    QString RequestPinCode(const QDBusObjectPath &) { return QStringLiteral("0000"); }
    void DisplayPinCode(const QDBusObjectPath &, const QString &) {}
    uint RequestPasskey(const QDBusObjectPath &) { return 0; }
    void DisplayPasskey(const QDBusObjectPath &, uint, ushort) {}
    void RequestConfirmation(const QDBusObjectPath &, uint) {}
    void RequestAuthorization(const QDBusObjectPath &) {}
    void AuthorizeService(const QDBusObjectPath &, const QString &) {}
    void Cancel() {}
};

class Bluetooth : public QObject {
    Q_OBJECT
    // off / starting / on / error
    Q_PROPERTY(QString state READ state NOTIFY changed)
    // 最近一件事的结果，显示在设置页上（可为空）
    Q_PROPERTY(QString message READ message NOTIFY changed)
    Q_PROPERTY(bool scanning READ scanning NOTIFY changed)
    // 选作麦克风的设备地址
    Q_PROPERTY(QString mic READ mic NOTIFY changed)
    Q_PROPERTY(QString busy READ busy NOTIFY changed)  // 正在配对的设备
    // [{path, address, name, paired, connected, audio}]
    Q_PROPERTY(QVariantList devices READ devices NOTIFY devicesChanged)

public:
    explicit Bluetooth(QObject *parent = nullptr) : QObject(parent), m_bus(QDBusConnection::systemBus()) {
        m_poll.setInterval(2000);
        connect(&m_poll, &QTimer::timeout, this, &Bluetooth::refresh);
        m_scanStop.setSingleShot(true);
        m_scanStop.setInterval(60000);
        connect(&m_scanStop, &QTimer::timeout, this, [this] { scan(false); });
        m_mic = settings().value(QStringLiteral("voice/mic")).toString();
        // 开机时若上次开着蓝牙就再打开；晚一点，不和 xochitl 的启动抢时间。
        if (settings().value(QStringLiteral("bluetooth/enabled")).toBool())
            QTimer::singleShot(8000, this, &Bluetooth::up);
    }

    QString state() const { return m_state; }
    QString message() const { return m_message; }
    bool scanning() const { return m_scanning; }
    QString mic() const { return m_mic; }
    QString busy() const { return m_busy; }
    QVariantList devices() const { return m_devices; }

    Q_INVOKABLE void setEnabled(bool on) {
        settings().setValue(QStringLiteral("bluetooth/enabled"), on);
        if (on) up();
        else down();
    }

    // 设置页打开时每 2 秒刷新一次设备列表。
    Q_INVOKABLE void watch(bool on) {
        m_watching = on;
        if (on && m_state == QLatin1String("on")) {
            refresh();
            m_poll.start();
        } else if (!on) {
            if (m_scanning) scan(false);
            m_poll.stop();
            set(m_state, QString());
        }
    }

    Q_INVOKABLE void scan(bool on) {
        if (m_state != QLatin1String("on")) return;
        if (on) {
            // 麦克风（耳机的通话功能）走经典蓝牙；低功耗蓝牙广播大多是周围的手机、手表。
            QVariantMap filter{{QStringLiteral("Transport"), QStringLiteral("bredr")}};
            call(bt::kAdapter, QStringLiteral("org.bluez.Adapter1"), QStringLiteral("SetDiscoveryFilter"), {filter});
            setProp(bt::kAdapter, QStringLiteral("org.bluez.Adapter1"), QStringLiteral("Pairable"), true);
            call(bt::kAdapter, QStringLiteral("org.bluez.Adapter1"), QStringLiteral("StartDiscovery"), {},
                 [this](const QDBusError &e) {
                     if (e.isValid() && e.name() != QLatin1String("org.bluez.Error.InProgress"))
                         set(m_state, QStringLiteral("无法搜索：%1").arg(e.message()));
                 });
            m_scanning = true;
            m_scanStop.start();
        } else {
            call(bt::kAdapter, QStringLiteral("org.bluez.Adapter1"), QStringLiteral("StopDiscovery"), {});
            m_scanning = false;
            m_scanStop.stop();
        }
        emit changed();
    }

    Q_INVOKABLE void pair(const QString &path) {
        if (!m_busy.isEmpty()) return;
        m_busy = path;
        set(m_state, QStringLiteral("正在配对…"));
        // 配对时停止搜索：搜索会占用无线电，让配对更慢、更容易失败。
        if (m_scanning) scan(false);
        QDBusMessage m = QDBusMessage::createMethodCall(bt::kService, path, QStringLiteral("org.bluez.Device1"),
                                                        QStringLiteral("Pair"));
        auto *w = new QDBusPendingCallWatcher(m_bus.asyncCall(m, 60000), this);
        connect(w, &QDBusPendingCallWatcher::finished, this, [this, path](QDBusPendingCallWatcher *w) {
            w->deleteLater();
            m_busy.clear();
            QDBusError e = w->error();
            if (e.isValid() && e.name() != QLatin1String("org.bluez.Error.AlreadyExists")) {
                logf("bluetooth: pair %s: %s", qPrintable(path), qPrintable(e.message()));
                set(m_state, QStringLiteral("配对没有成功（%1）。请确认麦克风处于配对模式后再试。").arg(e.message()));
                return;
            }
            setProp(path, QStringLiteral("org.bluez.Device1"), QStringLiteral("Trusted"), true);
            QString address = deviceAddress(path);
            if (m_mic.isEmpty() && isAudio(path)) useAsMic(address);
            set(m_state, QStringLiteral("已配对"));
            refresh();
        });
        emit changed();
    }

    Q_INVOKABLE void forget(const QString &path) {
        QString address = deviceAddress(path);
        call(bt::kAdapter, QStringLiteral("org.bluez.Adapter1"), QStringLiteral("RemoveDevice"),
             {QVariant::fromValue(QDBusObjectPath(path))}, [this](const QDBusError &) { refresh(); });
        if (address == m_mic) useAsMic(QString());
    }

    Q_INVOKABLE void useAsMic(const QString &address) {
        m_mic = address;
        settings().setValue(QStringLiteral("voice/mic"), address);
        emit changed();
        refresh();
    }

Q_SIGNALS:
    void changed();
    void devicesChanged();

private:
    static QSettings &settings() {
        static QSettings s(basePath("user/settings.ini"), QSettings::IniFormat);
        return s;
    }

    void set(const QString &state, const QString &message) {
        if (state == m_state && message == m_message) return;
        m_state = state;
        m_message = message;
        emit changed();
    }

    void up() {
        if (m_state == QLatin1String("on") || m_state == QLatin1String("starting")) return;
        set(QStringLiteral("starting"), QStringLiteral("正在打开蓝牙（首次约需 10 秒）…"));
        auto *p = new QProcess(this);
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.remove(QStringLiteral("LD_PRELOAD"));
        p->setProcessEnvironment(env);
        p->setProcessChannelMode(QProcess::MergedChannels);
        connect(p, &QProcess::finished, this, [this, p](int code, QProcess::ExitStatus) {
            QString out = QString::fromUtf8(p->readAll()).trimmed();
            p->deleteLater();
            if (code != 0) {
                logf("bluetooth: bluetooth-up.sh failed (%d): %s", code, qPrintable(out));
                set(QStringLiteral("error"), QStringLiteral("无法打开蓝牙：%1").arg(out));
                return;
            }
            m_tries = 0;
            powerOn();
        });
        p->start(QStringLiteral("/bin/sh"), {basePath("bluetooth-up.sh")});
    }

    // bluetoothd 刚启动、控制器的固件还在载入（约 5 秒）时会失败：每秒重试，最多 30 次。
    void powerOn() {
        QDBusMessage m = QDBusMessage::createMethodCall(bt::kService, bt::kAdapter, bt::kProps, QStringLiteral("Set"));
        m << QStringLiteral("org.bluez.Adapter1") << QStringLiteral("Powered") << QVariant::fromValue(QDBusVariant(true));
        auto *w = new QDBusPendingCallWatcher(m_bus.asyncCall(m), this);
        connect(w, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *w) {
            w->deleteLater();
            if (w->isError()) {
                if (++m_tries < 30) {
                    QTimer::singleShot(1000, this, &Bluetooth::powerOn);
                } else {
                    logf("bluetooth: power on: %s", qPrintable(w->error().message()));
                    set(QStringLiteral("error"), QStringLiteral("蓝牙控制器没有响应：%1").arg(w->error().message()));
                }
                return;
            }
            registerAgent();
            logf("bluetooth: on");
            set(QStringLiteral("on"), QString());
            if (m_watching) {
                refresh();
                m_poll.start();
            }
        });
    }

    void down() {
        if (m_scanning) scan(false);
        m_poll.stop();
        setProp(bt::kAdapter, QStringLiteral("org.bluez.Adapter1"), QStringLiteral("Powered"), false);
        m_devices.clear();
        emit devicesChanged();
        set(QStringLiteral("off"), QString());
        logf("bluetooth: off");
    }

    void registerAgent() {
        if (!m_agent) {
            m_agent = new BtAgent(this);
            if (!m_bus.registerObject(bt::kAgentPath, m_agent, QDBusConnection::ExportAllSlots))
                logf("bluetooth: cannot export the pairing agent: %s", qPrintable(m_bus.lastError().message()));
        }
        const QString mgr = QStringLiteral("org.bluez.AgentManager1");
        QDBusObjectPath agent(bt::kAgentPath);
        call(QStringLiteral("/org/bluez"), mgr, QStringLiteral("RegisterAgent"),
             {QVariant::fromValue(agent), QStringLiteral("NoInputNoOutput")}, [this, mgr, agent](const QDBusError &) {
                 call(QStringLiteral("/org/bluez"), mgr, QStringLiteral("RequestDefaultAgent"), {QVariant::fromValue(agent)});
             });
    }

    void call(const QString &path, const QString &iface, const QString &method, const QVariantList &args,
              std::function<void(const QDBusError &)> done = {}) {
        QDBusMessage m = QDBusMessage::createMethodCall(bt::kService, path, iface, method);
        m.setArguments(args);
        auto *w = new QDBusPendingCallWatcher(m_bus.asyncCall(m), this);
        connect(w, &QDBusPendingCallWatcher::finished, this, [done, method](QDBusPendingCallWatcher *w) {
            w->deleteLater();
            if (w->isError() && !done) logf("bluetooth: %s: %s", qPrintable(method), qPrintable(w->error().message()));
            if (done) done(w->error());
        });
    }

    void setProp(const QString &path, const QString &iface, const QString &name, const QVariant &value) {
        call(path, bt::kProps, QStringLiteral("Set"), {iface, name, QVariant::fromValue(QDBusVariant(value))});
    }

    // ---- 设备列表 -------------------------------------------------------------------
    struct Dev {
        QString path, address, name;
        bool paired = false, connected = false, audio = false;
        int rssi = -1000;
    };

    static QStringList stringList(const QVariant &v) {
        if (v.canConvert<QDBusArgument>()) {
            QStringList l;
            v.value<QDBusArgument>() >> l;
            return l;
        }
        return v.toStringList();
    }

    // 耳机的通话功能（HFP/HSP），或设备类别是音频设备。
    static bool audioDevice(const QVariantMap &p) {
        for (const QString &u : stringList(p.value(QStringLiteral("UUIDs")))) {
            QString s = u.left(8).toLower();
            if (s == QLatin1String("0000111e") || s == QLatin1String("00001108") || s == QLatin1String("00001131"))
                return true;
        }
        uint cls = p.value(QStringLiteral("Class")).toUInt();
        if (((cls >> 8) & 0x1f) == 0x04) return true;
        return p.value(QStringLiteral("Icon")).toString().startsWith(QLatin1String("audio"));
    }

    QString deviceAddress(const QString &path) const {
        for (const Dev &d : m_list)
            if (d.path == path) return d.address;
        return QString();
    }
    bool isAudio(const QString &path) const {
        for (const Dev &d : m_list)
            if (d.path == path) return d.audio;
        return false;
    }

    void refresh() {
        if (m_refreshing || m_state != QLatin1String("on")) return;
        m_refreshing = true;
        QDBusMessage m = QDBusMessage::createMethodCall(bt::kService, QStringLiteral("/"),
                                                        QStringLiteral("org.freedesktop.DBus.ObjectManager"),
                                                        QStringLiteral("GetManagedObjects"));
        auto *w = new QDBusPendingCallWatcher(m_bus.asyncCall(m), this);
        connect(w, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *w) {
            w->deleteLater();
            m_refreshing = false;
            QDBusMessage r = w->reply();
            if (w->isError() || r.arguments().isEmpty()) return;
            QList<Dev> list;
            const QDBusArgument arg = r.arguments().at(0).value<QDBusArgument>();
            arg.beginMap();
            while (!arg.atEnd()) {
                QDBusObjectPath path;
                arg.beginMapEntry();
                arg >> path;
                arg.beginMap();
                while (!arg.atEnd()) {
                    QString iface;
                    QVariantMap p;
                    arg.beginMapEntry();
                    arg >> iface >> p;
                    arg.endMapEntry();
                    if (iface != QLatin1String("org.bluez.Device1")) continue;
                    Dev d;
                    d.path = path.path();
                    d.address = p.value(QStringLiteral("Address")).toString();
                    d.name = p.value(QStringLiteral("Name")).toString();
                    d.paired = p.value(QStringLiteral("Paired")).toBool();
                    d.connected = p.value(QStringLiteral("Connected")).toBool();
                    d.audio = audioDevice(p);
                    if (p.contains(QStringLiteral("RSSI"))) d.rssi = p.value(QStringLiteral("RSSI")).toInt();
                    // 没有名字的多半是周围设备的匿名广播，列出来只会让人困惑。
                    if (!d.name.isEmpty() || d.paired) list << d;
                }
                arg.endMap();
                arg.endMapEntry();
            }
            arg.endMap();
            // 选用的麦克风、已配对的、音频设备在前，其余按信号强弱。
            std::sort(list.begin(), list.end(), [this](const Dev &a, const Dev &b) {
                auto rank = [this](const Dev &d) { return (d.address == m_mic) * 8 + d.paired * 4 + d.audio * 2; };
                if (rank(a) != rank(b)) return rank(a) > rank(b);
                return a.rssi > b.rssi;
            });
            m_list = list;
            QVariantList out;
            for (const Dev &d : list)
                out << QVariantMap{{QStringLiteral("path"), d.path},           {QStringLiteral("address"), d.address},
                                   {QStringLiteral("name"), d.name.isEmpty() ? d.address : d.name},
                                   {QStringLiteral("paired"), d.paired},       {QStringLiteral("connected"), d.connected},
                                   {QStringLiteral("audio"), d.audio}};
            // 列表没变就不通知：墨水屏上每次重绘都看得见。
            if (out != m_devices) {
                m_devices = out;
                emit devicesChanged();
            }
        });
    }

    QDBusConnection m_bus;
    BtAgent *m_agent = nullptr;
    QString m_state = QStringLiteral("off"), m_message, m_mic, m_busy;
    bool m_scanning = false, m_watching = false, m_refreshing = false;
    int m_tries = 0;
    QTimer m_poll, m_scanStop;
    QList<Dev> m_list;
    QVariantList m_devices;
};
