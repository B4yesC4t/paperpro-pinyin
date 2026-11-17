// PaperPro 拼音：语音识别（DeepSeek）。
//
// 整段上传：录好的音频（16 位 PCM WAV，最长 120 秒、8 MiB）一次 POST 到
// https://api.deepseek.com/v1/tools/asr，返回 {"text": "…"}。语言自动识别，
// 结果带标点。API Key 保存在安装目录的 user/deepseek-key（仅 root 可读）。
//
// 包含它的文件需先定义 logf() 与 basePath()。
#pragma once
#include <QtCore>
#include <QtNetwork>

class Voice : public QObject {
    Q_OBJECT
    // 已保存的 Key 的末四位，供设置页显示（没有 Key 时为空）
    Q_PROPERTY(QString keyHint READ keyHint NOTIFY changed)
    // idle / testing / ok / error
    Q_PROPERTY(QString testState READ testState NOTIFY changed)
    Q_PROPERTY(QString testMessage READ testMessage NOTIFY changed)

public:
    explicit Voice(QObject *parent = nullptr) : QObject(parent) {}

    static QString keyPath() { return basePath("user/deepseek-key"); }
    static QString key() {
        QFile f(keyPath());
        return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()).trimmed() : QString();
    }

    QString keyHint() const {
        QString k = key();
        return k.size() > 8 ? QStringLiteral("sk-…") + k.right(4) : QString();
    }
    QString testState() const { return m_testState; }
    QString testMessage() const { return m_testMessage; }

    // 上传 0.5 秒静音：验证 Key、网络和接口都正常（静音会得到空文本）。
    Q_INVOKABLE void testKey() {
        if (key().isEmpty()) {
            setTest(QStringLiteral("error"), QStringLiteral("还没有设置 API Key"));
            return;
        }
        setTest(QStringLiteral("testing"), QStringLiteral("正在连接 DeepSeek…"));
        recognize(wav(QByteArray(16000, 0), 16000), [this](bool ok, const QString &text, const QString &error) {
            Q_UNUSED(text);
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
            QJsonObject o = QJsonDocument::fromJson(body).object();
            if (reply->error() == QNetworkReply::NoError && status == 200) {
                done(true, o.value(QLatin1String("text")).toString(), QString());
                return;
            }
            QString why;
            switch (status) {
            case 401: why = QStringLiteral("API Key 无效"); break;
            case 402: why = QStringLiteral("DeepSeek 账户余额不足"); break;
            case 413: why = QStringLiteral("录音太长"); break;
            case 429: why = QStringLiteral("请求太频繁，请稍后再试"); break;
            case 503: case 504: why = QStringLiteral("DeepSeek 服务繁忙，请稍后再试"); break;
            case 0: why = QStringLiteral("无法连接 DeepSeek（%1），请检查 Wi-Fi").arg(reply->errorString()); break;
            default: why = QStringLiteral("识别失败（HTTP %1）").arg(status);
            }
            logf("voice: asr HTTP %d: %s", status, body.left(200).constData());
            done(false, QString(), why);
        });
    }

Q_SIGNALS:
    void changed();

private:
    QNetworkAccessManager *net() {
        if (!m_net) m_net = new QNetworkAccessManager(this);
        return m_net;
    }
    void setTest(const QString &state, const QString &message) {
        m_testState = state;
        m_testMessage = message;
        emit changed();
    }

    QNetworkAccessManager *m_net = nullptr;
    QString m_testState = QStringLiteral("idle"), m_testMessage;
};
