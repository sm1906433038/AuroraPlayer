// TranslationEngine.h
// Translates batches of subtitle text via any OpenAI-compatible chat
// completions API. Tested endpoints:
//
//   * DeepSeek      https://api.deepseek.com/v1/chat/completions
//   * OpenAI        https://api.openai.com/v1/chat/completions
//   * Moonshot      https://api.moonshot.cn/v1/chat/completions
//   * 智谱 GLM      https://open.bigmodel.cn/api/paas/v4/chat/completions
//   * 任何自建      只要遵循 OpenAI chat-completions schema
//
// Operates synchronously from a worker thread; uses a QEventLoop to block
// on QNetworkReply so the calling code stays simple.

#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <atomic>

class QNetworkAccessManager;

namespace promp::ai {

struct TranslationConfig {
    QString endpoint;             ///< full URL incl. /chat/completions
    QString apiKey;
    QString model;                ///< "deepseek-chat", "gpt-4o-mini", etc.
    QString targetLanguage = QStringLiteral("简体中文");
    QString sourceLanguageHint;   ///< optional, e.g. "日语"
    /// Optional override of the default system prompt. {target} / {source}
    /// placeholders get substituted.
    QString systemPromptOverride;

    /// How many SRT lines to bundle per request. More = fewer round-trips
    /// (cheaper, faster) but bigger risk a single failure ruins a batch.
    int batchSize = 20;

    /// Per-request timeout (ms).
    int timeoutMs = 60000;

    /// Sampling temperature. 0.0 = deterministic translation, 0.3 = a bit
    /// more natural Chinese, 0.7 = creative but risky.
    double temperature = 0.2;
};

/// Result of one translate() call.
struct TranslationBatchResult {
    bool        ok = false;
    QStringList translations;  ///< same length & order as input on success
    QString     error;
    int         httpStatus = 0;
};

class TranslationEngine {
public:
    explicit TranslationEngine(const TranslationConfig& cfg);
    ~TranslationEngine();

    /// Translate a batch of N strings synchronously. Returns N translations
    /// in the same order. Honours `cancel` between sub-steps.
    TranslationBatchResult translateBatch(const QStringList& sources,
                                          std::atomic<bool>& cancel);

    [[nodiscard]] const TranslationConfig& config() const noexcept { return m_cfg; }

private:
    TranslationConfig m_cfg;
    QNetworkAccessManager* m_net = nullptr;
};

} // namespace promp::ai
