// Copyright (c) 2026, Aegisub Project
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
// WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
// ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
// ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
// OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

/// @file ai_client.h
/// @brief Minimal multi-provider chat-completion client for the AI assistant.

#pragma once

#include <string>
#include <vector>

namespace ai {
	/// Which chat/model APIs are supported. OpenAI, OpenRouter and Ollama all
	/// speak the OpenAI /chat/completions and /models dialect; Anthropic and
	/// Google have their own request/response shapes.
	struct Config {
		std::string provider; ///< "OpenAI" / "OpenRouter" / "Anthropic" / "Google" / "Ollama"
		std::string endpoint; ///< Base URL, e.g. "https://api.openai.com/v1"
		std::string key;      ///< API key (may be empty, e.g. for Ollama)
		std::string model;    ///< Model id
	};

	/// Send a single-turn chat request and return the assistant's reply text.
	/// Blocks on network I/O, so call from a background thread. Throws
	/// std::runtime_error on any network/API/parse failure.
	std::string Chat(Config const& cfg, std::string const& system, std::string const& user);

	/// Fetch the list of available model ids for the configured provider.
	/// Blocks on network I/O. Throws std::runtime_error on failure.
	std::vector<std::string> ListModels(Config const& cfg);
}
