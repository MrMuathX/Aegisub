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

/// @file ai.cpp
/// @brief AI assistant commands.
/// @ingroup command

#include "command.h"

#include "../dialog_ai_assistant.h"
#include "../include/aegisub/context.h"

namespace {
	using cmd::Command;

	struct tool_ai_assistant final : public Command {
		CMD_NAME("tool/ai_assistant")
		STR_MENU("&AI Assistant...")
		STR_DISP("AI Assistant")
		STR_HELP("Edit subtitles with an AI model")

		void operator()(agi::Context *c) override {
			ShowAiAssistantDialog(c);
		}
	};
}

namespace cmd {
	void init_ai() {
		reg(std::make_unique<tool_ai_assistant>());
	}
}
