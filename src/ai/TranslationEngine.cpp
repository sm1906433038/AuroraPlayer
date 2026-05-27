#include "TranslationEngine.h"

#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QTimer>
#include <QUrl>

namespace promp::ai {

// ---------------------------------------------------------------------------

namespace {

/// Normalize partial endpoints the user pastes from provider docs.
///   "https://api.foo.com"              → "https://api.foo.com/v1/chat/completions"
///   "https://api.foo.com/"             → "https://api.foo.com/v1/chat/completions"
///   "https://api.foo.com/v1"           → "https://api.foo.com/v1/chat/completions"
///   "https://api.foo.com/v1/"          → "https://api.foo.com/v1/chat/completions"
///   "https://api.foo.com/v1/chat/..."  → (unchanged)
QString normaliseEndpoint(QString url) {
    url = url.trimmed();
    if (url.isEmpty()) return url;
    while (url.endsWith(QChar('/'))) url.chop(1);
    if (url.contains(QStringLiteral("/chat/completions"))) return url;
    if (url.endsWith(QStringLiteral("/v1"))
            || url.endsWith(QStringLiteral("/v4"))
            || url.endsWith(QStringLiteral("/api/paas/v4"))) {
        return url + QStringLiteral("/chat/completions");
    }
    return url + QStringLiteral("/v1/chat/completions");
}

QString defaultSystemPrompt(const QString& src, const QString& tgt) {
    return QStringLiteral(
        "You are a professional subtitle translator.\n"
        "The user will give you numbered lines like:\n"
        "  [1] source text\n"
        "  [2] source text\n"
        "Translate each line from %1 into %2.\n"
        "Output MUST use the EXACT same numbering format:\n"
        "  [1] translated text\n"
        "  [2] translated text\n"
        "Rules:\n"
        "- One input line → one output line, same [N] tag.\n"
        "- Never merge, split, reorder, or skip any line.\n"
        "- If a line is untranslatable, output it unchanged with its [N] tag.\n"
        "- Output ONLY the numbered translations, nothing else.")
        .arg(src.isEmpty() ? QStringLiteral("the source language") : src,
             tgt);
}

QString joinBatch(const QStringList& src) {
    QStringList numbered;
    numbered.reserve(src.size());
    for (int i = 0; i < src.size(); ++i) {
        numbered.append(QStringLiteral("[%1] %2").arg(i + 1).arg(src[i]));
    }
    return numbered.join(QChar('\n'));
}

QStringList splitTranslation(const QString& reply, int expected) {
    // Parse by [N] tags instead of raw line order — this is immune to
    // the model inserting extra blank lines, merging lines, or shifting.
    // We build a map from tag number → content, then fill the result
    // list in order.
    static const QRegularExpression tagRe(
        QStringLiteral("^\\s*\\[?(\\d+)\\]?[\\s\\.\\)、:：]*(.*)$"));

    QHash<int, QString> byTag;
    const auto lines = reply.split(QChar('\n'));
    for (const auto& raw : lines) {
        const QString line = raw.trimmed();
        if (line.isEmpty()) continue;
        auto m = tagRe.match(line);
        if (m.hasMatch()) {
            int idx = m.captured(1).toInt();
            QString text = m.captured(2).trimmed();
            if (idx >= 1 && idx <= expected) {
                byTag[idx] = text;
            }
        }
    }

    QStringList result;
    result.reserve(expected);
    for (int i = 1; i <= expected; ++i) {
        result.append(byTag.value(i));
    }
    return result;
}

} // namespace

// ---------------------------------------------------------------------------

TranslationEngine::TranslationEngine(const TranslationConfig& cfg)
    : m_cfg(cfg), m_net(new QNetworkAccessManager) {}

TranslationEngine::~TranslationEngine() { delete m_net; }

TranslationBatchResult TranslationEngine::translateBatch(
    const QStringList& sources, std::atomic<bool>& cancel) {

    TranslationBatchResult r;
    if (sources.isEmpty()) { r.ok = true; return r; }

    if (m_cfg.endpoint.isEmpty() || m_cfg.apiKey.isEmpty()
            || m_cfg.model.isEmpty()) {
        r.error = QStringLiteral("API endpoint / key / model 任意一项为空");
        return r;
    }

    QString system = m_cfg.systemPromptOverride.isEmpty()
                       ? defaultSystemPrompt(m_cfg.sourceLanguageHint,
                                             m_cfg.targetLanguage)
                       : m_cfg.systemPromptOverride;
    system.replace(QStringLiteral("{target}"), m_cfg.targetLanguage);
    system.replace(QStringLiteral("{source}"),
                   m_cfg.sourceLanguageHint.isEmpty()
                       ? QStringLiteral("the source language")
                       : m_cfg.sourceLanguageHint);

    QJsonArray messages;
    messages.append(QJsonObject{
        {"role",    "system"},
        {"content", system}});
    messages.append(QJsonObject{
        {"role",    "user"},
        {"content", joinBatch(sources)}});

    QJsonObject body{
        {"model",       m_cfg.model},
        {"messages",    messages},
        {"temperature", m_cfg.temperature},
        {"stream",      false},
    };

    const QString fullUrl = normaliseEndpoint(m_cfg.endpoint);
    QNetworkRequest req{QUrl(fullUrl)};
    req.setHeader(QNetworkRequest::ContentTypeHeader,
                  QStringLiteral("application/json"));
    req.setRawHeader("Authorization",
                     ("Bearer " + m_cfg.apiKey).toUtf8());
    req.setRawHeader("Accept", "application/json");
    req.setRawHeader("User-Agent", "AuroraPlayer/0.1 (subtitle-translate)");
    // OpenAI-compatible servers honor this; harmless when ignored.
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);

    const QByteArray payload =
        QJsonDocument(body).toJson(QJsonDocument::Compact);

    QNetworkReply* reply = m_net->post(req, payload);

    // Block the worker thread until reply finishes OR timeout fires OR
    // cancel flag flips. QEventLoop is a clean way to do sync HTTP from a
    // non-GUI thread without writing our own state machine.
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);

    QTimer cancelPoll;
    cancelPoll.setInterval(200);
    QObject::connect(&cancelPoll, &QTimer::timeout, [&]() {
        if (cancel.load()) { reply->abort(); loop.quit(); }
    });
    cancelPoll.start();

    timer.start(m_cfg.timeoutMs);
    loop.exec();
    cancelPoll.stop();

    r.httpStatus = reply->attribute(
                      QNetworkRequest::HttpStatusCodeAttribute).toInt();

    if (cancel.load()) {
        r.error = QStringLiteral("Cancelled");
        reply->deleteLater();
        return r;
    }
    if (!timer.isActive()) {
        r.error = QStringLiteral("请求超时（%1 ms）").arg(m_cfg.timeoutMs);
        reply->abort();
        reply->deleteLater();
        return r;
    }
    if (reply->error() != QNetworkReply::NoError) {
        const QByteArray errBody = reply->readAll();
        r.error = QStringLiteral("HTTP %1: %2 (%3)")
                      .arg(r.httpStatus)
                      .arg(reply->errorString(),
                           QString::fromUtf8(errBody.left(400)));
        reply->deleteLater();
        return r;
    }

    const QByteArray respBody = reply->readAll();
    reply->deleteLater();

    // If the server gave us HTML, the endpoint URL is almost certainly wrong
    // (typical mistake: pasting the docs homepage like "https://api.foo.com"
    // without the /v1/chat/completions tail). Flag this loudly.
    const QByteArray head = respBody.left(200).trimmed().toLower();
    if (head.startsWith("<!doctype") || head.startsWith("<html")) {
        r.error = QStringLiteral(
            "Endpoint 返回了 HTML，不是 JSON。请检查 URL 是否完整 —— 例如\n"
            "  ✓ https://api.deepseek.com/v1/chat/completions\n"
            "  ✗ https://api.deepseek.com\n"
            "我已经自动补全为：%1\n"
            "若仍报错，请把完整路径手填进去。").arg(fullUrl);
        return r;
    }

    QJsonParseError perr;
    const auto doc = QJsonDocument::fromJson(respBody, &perr);
    if (perr.error != QJsonParseError::NoError) {
        r.error = QStringLiteral("JSON 解析失败: %1\n%2")
                      .arg(perr.errorString(),
                           QString::fromUtf8(respBody.left(400)));
        return r;
    }
    const auto root = doc.object();
    // OpenAI schema: choices[0].message.content
    const auto choices = root.value(QStringLiteral("choices")).toArray();
    if (choices.isEmpty()) {
        r.error = QStringLiteral("响应缺少 choices: %1")
                      .arg(QString::fromUtf8(respBody.left(400)));
        return r;
    }
    const QString content = choices.first().toObject()
                                .value(QStringLiteral("message")).toObject()
                                .value(QStringLiteral("content")).toString();
    if (content.isEmpty()) {
        r.error = QStringLiteral("响应 content 为空");
        return r;
    }

    r.translations = splitTranslation(content, sources.size());
    r.ok = true;
    return r;
}

} // namespace promp::ai
