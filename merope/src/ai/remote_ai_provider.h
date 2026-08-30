// ai/remote_ai_provider.h - the production adapter behind c_ai_provider.
//
// The rules from ai_provider.h do not change here: the model still never sees
// the dataset, and it still never produces code. What changes is that the JSON
// it produces now arrives over the network instead of from a table of keywords,
// so everything it says is treated as a proposal from an untrusted source:
// column names that do not exist are dropped, confidences are clamped, and the
// plan still has to pass the validator before the engine reads a byte.
//
// Any model, from any of three wire formats, or from anything that speaks the
// OpenAI chat shape - Ollama, vLLM, LM Studio, OpenRouter - through --api-base.
#pragma once

#include "ai_provider.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace merope {

// The three request shapes worth knowing. Everything else that exists speaks
// one of them, which is why `custom` is not a fourth format but a base URL.
enum class ai_wire_t : std::uint8_t { mock, gemini, openai, anthropic };

struct ai_settings_t {
    std::string provider;                // mock | gemini | openai | anthropic
    std::string model;                   // any model id the provider will accept
    std::string api_key;
    std::string api_base;                // empty means the provider default
    int         timeout_seconds   = 60;
    int         max_output_tokens = 4096;
};

// What was chosen, and where it came from, so `merope profile` can say which
// model produced an answer without anyone having to guess. Never holds a key
// in a form meant for printing: use redact_key for that.
struct ai_resolution_t {
    ai_settings_t settings;
    std::string   source;        // flags | environment | <config path> | default
    std::string   note;          // why the mock is in use, when it is
    bool          remote = false;
};

ai_wire_t   wire_from_string(const std::string& provider) noexcept;
const char* to_string(ai_wire_t wire) noexcept;

// The endpoint a call goes to. Gemini puts the model and the verb in the path,
// which is why this takes the model rather than only the base.
std::string chat_endpoint(ai_wire_t wire, const std::string& api_base, const std::string& model);
std::string models_endpoint(ai_wire_t wire, const std::string& api_base);

// The default model per wire, used only when nobody named one.
const char* default_model(ai_wire_t wire) noexcept;

// Resolution order, strongest first: explicit settings (the command line),
// then the environment, then the config file, then the mock. A remote provider
// is never selected without a key: a 401 on every query is a worse outcome
// than the mock saying plainly that it is the mock.
ai_resolution_t resolve_ai(const ai_settings_t& requested, const std::string& config_path);

// The config file, which is where a key belongs: not on a command line, where
// it lands in shell history, and not in the repository.
bool load_ai_config(const std::string& path, ai_settings_t& out, std::string& error);
std::string default_config_path();

std::unique_ptr<c_ai_provider> make_ai_provider(const ai_settings_t& settings);

// Asks the provider which models the key can actually use. Returns false with a
// reason rather than a guess: a hardcoded list of model names goes stale.
bool list_models(const ai_settings_t& settings, std::vector<std::string>& out, std::string& error);

// ---- exposed for the self test, because each one is a way to be wrong ------

// A model told to answer with JSON often answers with JSON in a fence anyway.
std::string strip_code_fence(const std::string& text);

// The reply shape differs per wire; picking the wrong field yields an empty
// plan and a confusing error much later.
bool extract_reply_text(ai_wire_t wire, const std::string& body, std::string& text, std::string& error);

std::string build_chat_body(ai_wire_t wire, const ai_settings_t& settings,
                            const std::string& system, const std::string& user);

// Never the whole key, in any output: first three and last two characters.
std::string redact_key(const std::string& key);

// The two contracts the model is held to, and the only instructions it gets.
std::string schema_system_prompt();
std::string plan_system_prompt();

} // namespace merope
