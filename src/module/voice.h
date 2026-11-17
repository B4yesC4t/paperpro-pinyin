// PaperPro 拼音：语音输入。
//
// 流程与常见的开源语音输入工具一致：按键开始 → 录音 → 再按结束 → 语音识别 →
// 大模型整理 → 插入光标处。
//   录音：voice-hfp 子进程（蓝牙免提麦克风，8 kHz 单声道 PCM），见 src/helper/voice-hfp.c
//   识别：DeepSeek 语音识别 https://api.deepseek.com/v1/tools/asr（整段上传，最长 120 秒）
//   整理：DeepSeek Flash 修正错别字、同音字、标点，删去口头填充词，不改原意
// API Key 保存在安装目录的 user/deepseek-key（仅 root 可读）。
//
// 包含它的文件需先定义 logf() 与 basePath()。
#pragma once
#include <QtCore>
#include <QtNetwork>

class Voice : public QObject {
    Q_OBJECT
    // idle / connecting / ready / recording / recognizing / polishing / error
    Q_PROPERTY(QString state READ state NOTIFY changed)
    Q_PROPERTY(QString message READ message NOTIFY changed)
    // 录音开始的时刻（毫秒，Date.now() 的基准），供界面显示时长
    Q_PROPERTY(double recordingSince READ recordingSince NOTIFY changed)
    Q_PROPERTY(bool polish READ polish WRITE setPolish NOTIFY changed)
    // 整理用的提示词与热词，可在设置页编辑
    Q_PROPERTY(QString prompt READ prompt WRITE setPrompt NOTIFY changed)
    Q_PROPERTY(QString hotwords READ hotwords WRITE setHotwords NOTIFY changed)
    Q_PROPERTY(QString defaultPrompt READ defaultPrompt CONSTANT)
    // 设置页：已保存的 Key 的末四位（没有 Key 时为空）、连接测试
    Q_PROPERTY(QString keyHint READ keyHint NOTIFY changed)
    Q_PROPERTY(QString testState READ testState NOTIFY changed)
    Q_PROPERTY(QString testMessage READ testMessage NOTIFY changed)

public:
    explicit Voice(QObject *parent = nullptr) : QObject(parent) {
        m_idle.setSingleShot(true);
        connect(&m_idle, &QTimer::timeout, this, [this] {
            if (m_state == QLatin1String("ready")) disconnectMic();
        });
        m_limit.setSingleShot(true);
        m_limit.setInterval(110000);  // 接口上限 120 秒，留出余量
        connect(&m_limit, &QTimer::timeout, this, [this] {
            if (m_state == QLatin1String("recording")) stop();
        });
    }

    QString state() const { return m_state; }
    QString message() const { return m_message; }
    double recordingSince() const { return m_since; }
    bool polish() const { return settings().value(QStringLiteral("voice/polish"), true).toBool(); }
    void setPolish(bool on) {
        settings().setValue(QStringLiteral("voice/polish"), on);
        emit changed();
    }
    static QString defaultPrompt() {
        return QStringLiteral("修正语音转写中的同音字、明显错字和标点，在不改变原意、不增加事实、不删除有效信息的前提下，"
                              "让表达清晰自然。专有名词优先参考热词。只整理文字，不要总结，也不要回答转写中的问题。");
    }
    QString prompt() const {
        QString p = settings().value(QStringLiteral("voice/prompt")).toString().trimmed();
        return p.isEmpty() ? defaultPrompt() : p;
    }
    void setPrompt(const QString &p) {
        // 与默认相同就不存：以后默认提示词改进时，没改过的人自动用上新的。
        if (p.trimmed() == defaultPrompt() || p.trimmed().isEmpty()) settings().remove(QStringLiteral("voice/prompt"));
        else settings().setValue(QStringLiteral("voice/prompt"), p.trimmed());
        emit changed();
    }
    QString hotwords() const { return settings().value(QStringLiteral("voice/hotwords")).toString(); }
    void setHotwords(const QString &h) {
        settings().setValue(QStringLiteral("voice/hotwords"), h.trimmed());
        emit changed();
    }

    bool busy() const {
        return m_state == QLatin1String("recording") || m_state == QLatin1String("recognizing") ||
               m_state == QLatin1String("polishing") || (m_state == QLatin1String("connecting") && m_startWhenReady);
    }

    static QString keyPath() { return basePath("user/deepseek-key"); }
    static QString key() {
        QFile f(keyPath());
        return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()).trimmed() : QString();
    }
    QString keyHint() const {
        QString k = key();
        return k.size() > 8 ? QStringLiteral("sk-…") + k.right(4) : QString();
    }
    // 设置页保存 Key：只存在平板上（仅 root 可读），不会进入安装包或仓库。
    // 返回错误说明；成功时返回空字符串。空 Key 表示清除。
    Q_INVOKABLE QString setKey(const QString &value) {
        QString k = value.trimmed();
        if (k.isEmpty()) {
            QFile::remove(keyPath());
            emit changed();
            return QString();
        }
        if (k.contains(QRegularExpression(QStringLiteral("\\s"))) || k.size() < 20)
            return QStringLiteral("这不像是 DeepSeek 的 API Key（应以 sk- 开头，中间没有空格）");
        QDir().mkpath(basePath("user"));
        QFile f(keyPath());
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return QStringLiteral("无法保存：%1").arg(f.errorString());
        f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        f.write(k.toUtf8());
        f.close();
        m_testState = QStringLiteral("idle");
        m_testMessage.clear();
        emit changed();
        return QString();
    }
    QString testState() const { return m_testState; }
    QString testMessage() const { return m_testMessage; }

    // 进入文字编辑时预先连好麦克风：按下语音键时只需打开语音链路。离开编辑
    // 5 分钟后断开，省电。
    void editing(bool on, const QString &mic) {
        if (on) {
            m_idle.stop();
            if (!mic.isEmpty() && m_state == QLatin1String("idle")) connectMic(mic);
        } else if (m_state == QLatin1String("ready")) {
            m_idle.start(5 * 60 * 1000);
        }
    }

    // 开始 / 结束（同一个键）。
    void toggle(const QString &mic) {
        if (m_state == QLatin1String("recording")) {
            stop();
        } else if (m_state == QLatin1String("recognizing") || m_state == QLatin1String("polishing")) {
            return;
        } else {
            start(mic);
        }
    }

    void start(const QString &mic) {
        if (mic.isEmpty()) {
            fail(QStringLiteral("还没有选择麦克风：在“输入法设置”里配对并选用"));
            return;
        }
        if (key().isEmpty()) {
            fail(QStringLiteral("还没有设置 DeepSeek API Key"));
            return;
        }
        m_pcm.clear();
        if (m_connected) {
            send("start");
            set(QStringLiteral("recording"), QString());
            m_since = double(QDateTime::currentMSecsSinceEpoch());
            m_limit.start();
            emit changed();
            return;
        }
        m_startWhenReady = true;
        if (m_state != QLatin1String("connecting")) connectMic(mic);
    }

    void stop() {
        m_limit.stop();
        if (m_startWhenReady) {  // 还没连上麦克风就松开了
            cancel();
            return;
        }
        if (m_state != QLatin1String("recording")) return;
        send("stop");
        set(QStringLiteral("recognizing"), QString());
        // 剩余的音频会在 "S stopped" 之前到达；识别在那时开始。
    }

    void cancel() {
        m_limit.stop();
        m_startWhenReady = false;
        if (m_state == QLatin1String("recording")) send("stop");
        if (m_state == QLatin1String("connecting")) disconnectMic();
        set(m_connected ? QStringLiteral("ready") : QStringLiteral("idle"), QString());
    }

    // ---- 设置页 ----------------------------------------------------------------------
    // 上传 0.5 秒静音：验证 Key、网络和接口都正常（静音会得到空文本）。
    Q_INVOKABLE void testKey() {
        if (key().isEmpty()) {
            setTest(QStringLiteral("error"), QStringLiteral("还没有设置 API Key"));
            return;
        }
        setTest(QStringLiteral("testing"), QStringLiteral("正在连接 DeepSeek…"));
        recognize(wav(QByteArray(8000, 0), 8000), [this](bool ok, const QString &, const QString &error) {
            if (ok) setTest(QStringLiteral("ok"), QStringLiteral("连接正常"));
            else setTest(QStringLiteral("error"), error);
        });
    }

    // 16 位单声道 PCM 包成 WAV。
    static QByteArray wav(const QByteArray &pcm, int rate) {
        QByteArray h;
        QDataStream s(&h, QIODevice::WriteOnly);
        s.setByteOrder(QDataStream::LittleEndian);
        s.writeRawData("RIFF", 4);
        s << quint32(36 + pcm.size());
        s.writeRawData("WAVEfmt ", 8);
        s << quint32(16) << quint16(1) << quint16(1) << quint32(rate) << quint32(rate * 2) << quint16(2) << quint16(16);
        s.writeRawData("data", 4);
        s << quint32(pcm.size());
        return h + pcm;
    }

Q_SIGNALS:
    void changed();
    void text(const QString &text);  // 识别并整理好的文字，插入光标处

private:
    static QSettings &settings() {
        static QSettings s(basePath("user/settings.ini"), QSettings::IniFormat);
        return s;
    }

    void set(const QString &state, const QString &message) {
        m_state = state;
        m_message = message;
        emit changed();
    }
    void fail(const QString &why) {
        logf("voice: %s", qPrintable(why));
        m_startWhenReady = false;
        set(QStringLiteral("error"), why);
        // 错误提示停留几秒后消失。
        QTimer::singleShot(4000, this, [this] {
            if (m_state == QLatin1String("error")) set(m_connected ? QStringLiteral("ready") : QStringLiteral("idle"), QString());
        });
    }
    void send(const char *cmd) {
        if (m_proc) m_proc->write(QByteArray(cmd) + '\n');
    }

    // 麦克风免提服务的 RFCOMM 通道：从 bluetoothd 缓存的 SDP 记录里找。
    static int channel(const QString &mic) {
        for (const QString &dir : QDir(QStringLiteral("/var/lib/bluetooth")).entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            QSettings cache(QStringLiteral("/var/lib/bluetooth/%1/cache/%2").arg(dir, mic), QSettings::IniFormat);
            cache.beginGroup(QStringLiteral("ServiceRecords"));
            for (const QString &k : cache.childKeys()) {
                QString rec = cache.value(k).toString().toUpper();
                // 服务类别里有 Handsfree（0x111E）的记录；协议描述里 RFCOMM（0x0003）后的通道号。
                if (!rec.contains(QLatin1String("19111E"))) continue;
                int i = rec.indexOf(QLatin1String("19000308"));
                if (i >= 0) return rec.mid(i + 8, 2).toInt(nullptr, 16);
            }
        }
        return 0;
    }

    void connectMic(const QString &mic) {
        int ch = channel(mic);
        if (!ch) {
            fail(QStringLiteral("这个麦克风没有免提服务（HFP），不能用作语音输入"));
            return;
        }
        disconnectMic();
        m_mic = mic;
        set(QStringLiteral("connecting"), QString());
        m_proc = new QProcess(this);
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.remove(QStringLiteral("LD_PRELOAD"));
        m_proc->setProcessEnvironment(env);
        m_proc->setProcessChannelMode(QProcess::ForwardedErrorChannel);
        QProcess *p = m_proc;
        connect(p, &QProcess::readyReadStandardOutput, this, [this, p] { if (p == m_proc) readHelper(); });
        connect(p, &QProcess::finished, this, [this, p] {
            p->deleteLater();
            if (p != m_proc) return;
            m_proc = nullptr;
            m_connected = false;
            bool wasBusy = m_state == QLatin1String("recording") || m_startWhenReady;
            if (m_state == QLatin1String("error")) return;
            if (wasBusy) fail(QStringLiteral("麦克风断开了"));
            else if (m_state != QLatin1String("recognizing") && m_state != QLatin1String("polishing"))
                set(QStringLiteral("idle"), QString());
        });
        p->start(basePath("voice-hfp"), {mic, QString::number(ch)});
        logf("voice: connecting to %s (channel %d)", qPrintable(mic), ch);
    }

    void disconnectMic() {
        if (!m_proc) return;
        QProcess *p = m_proc;
        m_proc = nullptr;
        m_connected = false;
        p->write("quit\n");
        p->closeWriteChannel();
        QTimer::singleShot(3000, p, [p] { if (p->state() != QProcess::NotRunning) p->kill(); });
        if (m_state == QLatin1String("ready") || m_state == QLatin1String("connecting")) set(QStringLiteral("idle"), QString());
    }

    // 子进程的帧：S <状态> / A <字节数> + PCM
    void readHelper() {
        m_in += m_proc->readAllStandardOutput();
        for (;;) {
            if (m_need > 0) {
                if (m_in.size() < m_need) return;
                if (m_state == QLatin1String("recording") || m_state == QLatin1String("recognizing"))
                    m_pcm += m_in.left(m_need);
                m_in.remove(0, m_need);
                m_need = 0;
                continue;
            }
            int nl = m_in.indexOf('\n');
            if (nl < 0) return;
            QByteArray line = m_in.left(nl);
            m_in.remove(0, nl + 1);
            if (line.startsWith("A ")) {
                m_need = line.mid(2).toInt();
            } else if (line == "S ready") {
                logf("voice: microphone connected");
                m_connected = true;
                if (m_state == QLatin1String("connecting")) set(QStringLiteral("ready"), QString());
                if (m_startWhenReady) {
                    m_startWhenReady = false;
                    start(m_mic);
                }
            } else if (line == "S recording") {
                // 第一段音频到了。
            } else if (line == "S stopped") {
                if (m_state == QLatin1String("recognizing")) finish();
                else if (m_state == QLatin1String("recording")) {  // 麦克风那边关了语音链路
                    m_limit.stop();
                    set(QStringLiteral("recognizing"), QString());
                    finish();
                }
            } else if (line.startsWith("S error ")) {
                fail(QString::fromUtf8(line.mid(8)));
            }
        }
    }

    void finish() {
        QByteArray pcm = m_pcm;
        m_pcm.clear();
        double seconds = pcm.size() / 16000.0;
        logf("voice: recorded %.1f s", seconds);
        if (seconds < 0.4) {
            fail(QStringLiteral("没有录到声音"));
            return;
        }
        recognize(wav(pcm, 8000), [this](bool ok, const QString &raw, const QString &error) {
            if (!ok) {
                fail(error);
                return;
            }
            QString t = raw.trimmed();
            logf("voice: recognized %d characters", int(t.size()));
            if (t.isEmpty()) {
                fail(QStringLiteral("没有识别出文字"));
                return;
            }
            if (!polish()) {
                deliver(t);
                return;
            }
            set(QStringLiteral("polishing"), QString());
            tidy(t, [this, t](const QString &done) { deliver(done.isEmpty() ? t : done); });
        });
    }

    void deliver(const QString &t) {
        set(m_connected ? QStringLiteral("ready") : QStringLiteral("idle"), QString());
        emit text(t);
    }

    QNetworkAccessManager *net() {
        if (!m_net) m_net = new QNetworkAccessManager(this);
        return m_net;
    }

    void recognize(const QByteArray &wavData, std::function<void(bool, const QString &, const QString &)> done) {
        QNetworkRequest req(QUrl(QStringLiteral("https://api.deepseek.com/v1/tools/asr")));
        req.setRawHeader("Authorization", "Bearer " + key().toUtf8());
        req.setTransferTimeout(60000);
        auto *multi = new QHttpMultiPart(QHttpMultiPart::FormDataType);
        QHttpPart file;
        file.setHeader(QNetworkRequest::ContentDispositionHeader,
                       QStringLiteral("form-data; name=\"file\"; filename=\"speech.wav\""));
        file.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("audio/wav"));
        file.setBody(wavData);
        multi->append(file);
        QNetworkReply *reply = net()->post(req, multi);
        multi->setParent(reply);
        connect(reply, &QNetworkReply::finished, this, [reply, done] {
            reply->deleteLater();
            int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            QByteArray body = reply->readAll();
            if (reply->error() == QNetworkReply::NoError && status == 200) {
                done(true, QJsonDocument::fromJson(body).object().value(QLatin1String("text")).toString(), QString());
                return;
            }
            logf("voice: asr HTTP %d: %s", status, body.left(200).constData());
            done(false, QString(), httpError(status, reply));
        });
    }

    // 大模型整理：失败或超时就用原始识别结果，不让一次网络问题吞掉整段话。
    void tidy(const QString &raw, std::function<void(const QString &)> done) {
        // 用户的提示词（设置页可改）＋热词＋固定的输出要求（让返回的就是可上屏的文字）。
        QString prompt = QStringLiteral("下面的用户消息是一段语音转写。") + this->prompt();
        QStringList words = hotwords().split(QRegularExpression(QStringLiteral("[\\s,，、;；]+")), Qt::SkipEmptyParts);
        if (!words.isEmpty()) prompt += QStringLiteral("\n热词：") + words.join(QStringLiteral("、"));
        prompt += QStringLiteral("\n输出：只输出整理后的文字本身，不加引号、说明或前缀。");
        QJsonObject body{
            {QStringLiteral("model"), QStringLiteral("deepseek-flash")},
            {QStringLiteral("reasoning_effort"), QStringLiteral("low")},
            {QStringLiteral("temperature"), 0},
            {QStringLiteral("messages"),
             QJsonArray{QJsonObject{{QStringLiteral("role"), QStringLiteral("system")}, {QStringLiteral("content"), prompt}},
                        QJsonObject{{QStringLiteral("role"), QStringLiteral("user")}, {QStringLiteral("content"), raw}}}}};
        QNetworkRequest req(QUrl(QStringLiteral("https://api.deepseek.com/chat/completions")));
        req.setRawHeader("Authorization", "Bearer " + key().toUtf8());
        req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
        req.setTransferTimeout(15000);
        QNetworkReply *reply = net()->post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
        connect(reply, &QNetworkReply::finished, this, [reply, done] {
            reply->deleteLater();
            int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            QByteArray b = reply->readAll();
            QString out;
            if (reply->error() == QNetworkReply::NoError && status == 200)
                out = QJsonDocument::fromJson(b)
                          .object()
                          .value(QLatin1String("choices"))
                          .toArray()
                          .at(0)
                          .toObject()
                          .value(QLatin1String("message"))
                          .toObject()
                          .value(QLatin1String("content"))
                          .toString()
                          .trimmed();
            else
                logf("voice: polish HTTP %d: %s", status, b.left(200).constData());
            done(out);
        });
    }

    static QString httpError(int status, QNetworkReply *reply) {
        switch (status) {
        case 401: return QStringLiteral("API Key 无效");
        case 402: return QStringLiteral("DeepSeek 账户余额不足");
        case 413: return QStringLiteral("录音太长");
        case 429: return QStringLiteral("请求太频繁，请稍后再试");
        case 503: case 504: return QStringLiteral("DeepSeek 服务繁忙，请稍后再试");
        case 0: return QStringLiteral("无法连接 DeepSeek（%1），请检查 Wi-Fi").arg(reply->errorString());
        default: return QStringLiteral("识别失败（HTTP %1）").arg(status);
        }
    }

    void setTest(const QString &state, const QString &message) {
        m_testState = state;
        m_testMessage = message;
        emit changed();
    }

    QProcess *m_proc = nullptr;
    QNetworkAccessManager *m_net = nullptr;
    QString m_state = QStringLiteral("idle"), m_message, m_mic;
    QString m_testState = QStringLiteral("idle"), m_testMessage;
    QByteArray m_in, m_pcm;
    int m_need = 0;
    double m_since = 0;
    bool m_startWhenReady = false, m_connected = false;
    QTimer m_idle, m_limit;
};
