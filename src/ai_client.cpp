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

/// @file ai_client.cpp
/// @brief Minimal multi-provider chat-completion client for the AI assistant.

#include "ai_client.h"

#include <libaegisub/cajun/elements.h>
#include <libaegisub/cajun/writer.h>
#include <libaegisub/json.h>

#include <curl/curl.h>
#include <sstream>
#include <stdexcept>

namespace {
	bool uses_openai_dialect(std::string const& provider) {
		return provider != "Anthropic" && provider != "Google";
	}

	size_t write_cb(char *contents, size_t size, size_t nmemb, std::string *s) {
		s->append(contents, size * nmemb);
		return size * nmemb;
	}

	int progress_cb(void *cancel, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
		auto flag = static_cast<std::atomic<bool> const*>(cancel);
		return flag && *flag ? 1 : 0;
	}

	// Perform an HTTP request. If body is non-null the request is a POST with
	// that body, otherwise a GET. Returns the response body; throws on error,
	// or ai::Cancelled if the cancel flag is raised mid-request.
	std::string http_request(std::string const& url, std::vector<std::string> const& headers, std::string const* body, std::atomic<bool> const* cancel) {
		if (cancel && *cancel)
			throw ai::Cancelled();

		CURL *curl = curl_easy_init();
		if (!curl)
			throw std::runtime_error("Could not initialize network library.");

		struct curl_slist *hdr = nullptr;
		for (auto const& h : headers)
			hdr = curl_slist_append(hdr, h.c_str());
		if (hdr)
			curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdr);

		curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
		curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
		curl_easy_setopt(curl, CURLOPT_USERAGENT, "Aegisub");
		if (body) {
			curl_easy_setopt(curl, CURLOPT_POST, 1L);
			curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body->c_str());
			curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body->size()));
		}

		std::string result;
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, &result);
		if (cancel) {
			curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
			curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_cb);
			curl_easy_setopt(curl, CURLOPT_XFERINFODATA, const_cast<std::atomic<bool> *>(cancel));
		}

		CURLcode rc = curl_easy_perform(curl);
		long status = 0;
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
		if (hdr)
			curl_slist_free_all(hdr);
		curl_easy_cleanup(curl);

		if (rc == CURLE_ABORTED_BY_CALLBACK)
			throw ai::Cancelled();
		if (rc != CURLE_OK)
			throw std::runtime_error(std::string("Network error: ") + curl_easy_strerror(rc));
		if (status < 200 || status >= 300) {
			std::string msg = result.substr(0, 500);
			if (status == 401 || status == 403)
				throw std::runtime_error("Authentication failed (HTTP " + std::to_string(status) + "): check the API key for this provider. " + msg);
			throw std::runtime_error("HTTP error " + std::to_string(status) + ": " + msg);
		}
		return result;
	}

	json::UnknownElement parse(std::string const& body) {
		std::istringstream ss(body);
		return agi::json_util::parse(ss);
	}

	std::string bearer(std::string const& key) { return "Authorization: Bearer " + key; }
}

namespace ai {
	std::string Chat(Config const& cfg, std::string const& system, std::string const& user) {
		std::vector<std::string> headers = {"Content-Type: application/json"};
		std::string url;
		std::ostringstream os;

		if (uses_openai_dialect(cfg.provider)) {
			if (!cfg.key.empty())
				headers.push_back(bearer(cfg.key));
			url = cfg.endpoint + "/chat/completions";

			json::Object sys_msg, user_msg;
			sys_msg["role"] = json::String("system");
			sys_msg["content"] = json::String(system);
			user_msg["role"] = json::String("user");
			user_msg["content"] = json::String(user);
			json::Array messages;
			messages.push_back(std::move(sys_msg));
			messages.push_back(std::move(user_msg));

			json::Object body;
			body["model"] = json::String(cfg.model);
			body["messages"] = std::move(messages);
			agi::JsonWriter::Write(body, os);
		}
		else if (cfg.provider == "Anthropic") {
			headers.push_back("x-api-key: " + cfg.key);
			headers.push_back("anthropic-version: 2023-06-01");
			url = cfg.endpoint + "/messages";

			json::Object user_msg;
			user_msg["role"] = json::String("user");
			user_msg["content"] = json::String(user);
			json::Array messages;
			messages.push_back(std::move(user_msg));

			json::Object body;
			body["model"] = json::String(cfg.model);
			body["max_tokens"] = json::Integer(2048);
			body["system"] = json::String(system);
			body["messages"] = std::move(messages);
			agi::JsonWriter::Write(body, os);
		}
		else { // Google
			url = cfg.endpoint + "/models/" + cfg.model + ":generateContent?key=" + cfg.key;

			json::Object user_part;
			user_part["text"] = json::String(user);
			json::Array user_parts;
			user_parts.push_back(std::move(user_part));
			json::Object content;
			content["parts"] = std::move(user_parts);
			json::Array contents;
			contents.push_back(std::move(content));

			json::Object sys_part;
			sys_part["text"] = json::String(system);
			json::Array sys_parts;
			sys_parts.push_back(std::move(sys_part));
			json::Object sys_instruction;
			sys_instruction["parts"] = std::move(sys_parts);

			json::Object body;
			body["contents"] = std::move(contents);
			body["systemInstruction"] = std::move(sys_instruction);
			agi::JsonWriter::Write(body, os);
		}

		std::string request = os.str();
		std::string response = http_request(url, headers, &request, cfg.cancel);

		try {
			json::UnknownElement ue = parse(response);
			json::Object const& root = ue;
			if (uses_openai_dialect(cfg.provider)) {
				json::Array const& choices = root.at("choices");
				json::Object const& first = choices.at(0);
				json::Object const& message = first.at("message");
				json::String const& text = message.at("content");
				return text;
			}
			else if (cfg.provider == "Anthropic") {
				json::Array const& content = root.at("content");
				json::Object const& first = content.at(0);
				json::String const& text = first.at("text");
				return text;
			}
			else { // Google
				json::Array const& candidates = root.at("candidates");
				json::Object const& first = candidates.at(0);
				json::Object const& content = first.at("content");
				json::Array const& parts = content.at("parts");
				json::Object const& part0 = parts.at(0);
				json::String const& text = part0.at("text");
				return text;
			}
		}
		catch (std::exception const&) {
			throw std::runtime_error("Unexpected response from provider: " + response.substr(0, 500));
		}
	}

	std::vector<std::string> ListModels(Config const& cfg) {
		std::vector<std::string> headers = {"Content-Type: application/json"};
		std::string url;

		if (uses_openai_dialect(cfg.provider)) {
			if (!cfg.key.empty())
				headers.push_back(bearer(cfg.key));
			url = cfg.endpoint + "/models";
		}
		else if (cfg.provider == "Anthropic") {
			headers.push_back("x-api-key: " + cfg.key);
			headers.push_back("anthropic-version: 2023-06-01");
			url = cfg.endpoint + "/models";
		}
		else { // Google
			url = cfg.endpoint + "/models?key=" + cfg.key;
		}

		std::string response = http_request(url, headers, nullptr, cfg.cancel);

		std::vector<std::string> models;
		try {
			json::UnknownElement ue = parse(response);
			json::Object const& root = ue;
			if (cfg.provider == "Google") {
				json::Array const& arr = root.at("models");
				for (size_t i = 0; i < arr.size(); ++i) {
					json::Object const& o = arr.at(i);
					json::String const& name = o.at("name");
					std::string id = name;
					// Names look like "models/gemini-1.5-pro"; keep the tail.
					auto slash = id.rfind('/');
					if (slash != std::string::npos)
						id = id.substr(slash + 1);
					models.push_back(id);
				}
			}
			else {
				json::Array const& arr = root.at("data");
				for (size_t i = 0; i < arr.size(); ++i) {
					json::Object const& o = arr.at(i);
					json::String const& id = o.at("id");
					models.push_back(id);
				}
			}
		}
		catch (std::exception const&) {
			throw std::runtime_error("Unexpected model list from provider: " + response.substr(0, 500));
		}

		return models;
	}
}
