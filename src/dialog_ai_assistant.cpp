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

/// @file dialog_ai_assistant.cpp
/// @brief AI assistant dialog.

#include "dialog_ai_assistant.h"

#include "ai_client.h"
#include "ass_dialogue.h"
#include "ass_file.h"
#include "compat.h"
#include "include/aegisub/context.h"
#include "options.h"
#include "selection_controller.h"

#include <libaegisub/dispatch.h>

#include <string>
#include <utility>
#include <vector>

#include <wx/button.h>
#include <wx/choice.h>
#include <wx/combobox.h>
#include <wx/dialog.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/translation.h>

namespace {
	const char *providers[] = {"OpenAI", "OpenRouter", "Anthropic", "Google", "Ollama"};

	struct Action {
		const char *label;
		const char *prompt;
	};

	const Action actions[] = {
		{"Fix long lines", "You are a subtitle editor. Rewrite the subtitle line to be shorter and easier to read quickly, without losing meaning or changing tone."},
		{"Spelling & grammar", "Correct spelling, punctuation and grammar in the subtitle line. Do not change the meaning, wording style or language."},
		{"Rephrase (shorter)", "Rephrase the subtitle line to be more concise and natural while preserving the original meaning."},
		{"Custom instruction", ""},
	};

	const char *system_suffix =
		"\nRespond with ONLY the resulting subtitle line text: no quotes, no "
		"explanation, no markdown. Preserve any ASS override tags such as {\\i1}. "
		"If no change is needed, return the line unchanged.";

	std::string trim(std::string s) {
		const char *ws = " \t\r\n";
		auto start = s.find_first_not_of(ws);
		if (start == std::string::npos) return "";
		auto end = s.find_last_not_of(ws);
		s = s.substr(start, end - start + 1);
		// Strip a single pair of surrounding quotes the model may add.
		if (s.size() >= 2 && ((s.front() == '"' && s.back() == '"') || (s.front() == '\'' && s.back() == '\'')))
			s = s.substr(1, s.size() - 2);
		return s;
	}

	class DialogAiAssistant final : public wxDialog {
		agi::Context *c;

		wxChoice *provider_choice;
		wxComboBox *model_combo;
		wxButton *refresh_button;
		wxChoice *scope_choice;
		wxChoice *action_choice;
		wxTextCtrl *instruction;
		wxButton *run_button;
		wxButton *cancel_button;
		wxStaticText *status;

		bool busy = false;

		ai::Config MakeConfig() {
			ai::Config cfg;
			cfg.provider = OPT_GET("AI/Provider")->GetString();
			cfg.endpoint = OPT_GET("AI/Endpoints/" + cfg.provider)->GetString();
			if (cfg.provider != "Ollama")
				cfg.key = OPT_GET("AI/Keys/" + cfg.provider)->GetString();
			cfg.model = from_wx(model_combo->GetValue());
			return cfg;
		}

		void SetBusy(bool b) {
			busy = b;
			provider_choice->Enable(!b);
			model_combo->Enable(!b);
			refresh_button->Enable(!b);
			scope_choice->Enable(!b);
			action_choice->Enable(!b);
			instruction->Enable(!b);
			run_button->Enable(!b);
			cancel_button->Enable(!b);
		}

		std::vector<AssDialogue *> TargetLines() {
			std::vector<AssDialogue *> lines;
			switch (scope_choice->GetSelection()) {
				case 1:
					for (auto& diag : c->ass->Events)
						lines.push_back(&diag);
					break;
				case 2:
					if (auto active = c->selectionController->GetActiveLine())
						lines.push_back(active);
					break;
				default:
					for (auto line : c->selectionController->GetSortedSelection())
						lines.push_back(line);
					break;
			}
			return lines;
		}

		void OnProviderChange(wxCommandEvent&) {
			OPT_SET("AI/Provider")->SetString(providers[provider_choice->GetSelection()]);
			model_combo->Clear();
			model_combo->SetValue(to_wx(OPT_GET("AI/Model")->GetString()));
		}

		void OnModelChange(wxCommandEvent&) {
			OPT_SET("AI/Model")->SetString(from_wx(model_combo->GetValue()));
		}

		void OnActionChange(wxCommandEvent&) {
			instruction->SetValue(to_wx(actions[action_choice->GetSelection()].prompt));
		}

		void OnRefresh(wxCommandEvent&) {
			if (busy) return;
			SetBusy(true);
			status->SetLabel(_("Fetching models..."));
			ai::Config cfg = MakeConfig();
			agi::dispatch::Background().Async([=, this] {
				std::vector<std::string> models;
				std::string error;
				try {
					models = ai::ListModels(cfg);
				}
				catch (std::exception const& e) {
					error = e.what();
				}
				agi::dispatch::Main().Async([=, this] {
					if (!error.empty()) {
						status->SetLabel(to_wx(error));
					}
					else {
						wxString current = model_combo->GetValue();
						model_combo->Clear();
						for (auto const& m : models)
							model_combo->Append(to_wx(m));
						model_combo->SetValue(current);
						status->SetLabel(wxString::Format(_("Loaded %d models."), (int)models.size()));
					}
					SetBusy(false);
				});
			});
		}

		void OnRun(wxCommandEvent&) {
			if (busy) return;

			auto lines = TargetLines();
			if (lines.empty()) {
				status->SetLabel(_("No lines to process."));
				return;
			}

			ai::Config cfg = MakeConfig();
			if (cfg.model.empty()) {
				status->SetLabel(_("Choose a model first."));
				return;
			}

			std::string sys = from_wx(instruction->GetValue());
			sys += system_suffix;

			SetBusy(true);
			status->SetLabel(wxString::Format(_("Processing %d line(s)..."), (int)lines.size()));

			agi::dispatch::Background().Async([=, this] {
				std::vector<std::pair<AssDialogue *, std::string>> edits;
				std::string error;
				try {
					for (auto line : lines) {
						std::string in = line->Text.get();
						std::string out = trim(ai::Chat(cfg, sys, in));
						if (!out.empty())
							edits.emplace_back(line, out);
					}
				}
				catch (std::exception const& e) {
					error = e.what();
				}
				agi::dispatch::Main().Async([=, this] {
					if (!error.empty()) {
						status->SetLabel(to_wx(error));
					}
					else {
						for (auto& e : edits)
							e.first->Text = e.second;
						c->ass->Commit(_("AI edit"), AssFile::COMMIT_DIAG_TEXT);
						status->SetLabel(wxString::Format(_("Updated %d line(s)."), (int)edits.size()));
					}
					SetBusy(false);
				});
			});
		}

	public:
		DialogAiAssistant(agi::Context *context)
		: wxDialog(nullptr, -1, _("AI Assistant"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
		, c(context)
		{
			auto main_sizer = new wxBoxSizer(wxVERTICAL);
			auto grid = new wxFlexGridSizer(2, FromDIP(wxSize(5, 5)));
			grid->AddGrowableCol(1, 1);

			wxArrayString provider_names;
			for (auto p : providers) provider_names.Add(p);
			provider_choice = new wxChoice(this, -1, wxDefaultPosition, wxDefaultSize, provider_names);
			std::string cur_provider = OPT_GET("AI/Provider")->GetString();
			provider_choice->SetSelection(0);
			for (size_t i = 0; i < provider_names.size(); ++i)
				if (from_wx(provider_names[i]) == cur_provider)
					provider_choice->SetSelection((int)i);

			model_combo = new wxComboBox(this, -1, to_wx(OPT_GET("AI/Model")->GetString()), wxDefaultPosition, FromDIP(wxSize(280, -1)), 0, nullptr, wxCB_DROPDOWN | wxTE_PROCESS_ENTER);
			refresh_button = new wxButton(this, -1, _("Refresh"));
			auto model_sizer = new wxBoxSizer(wxHORIZONTAL);
			model_sizer->Add(model_combo, wxSizerFlags(1).Expand());
			model_sizer->Add(refresh_button, wxSizerFlags().Border(wxLEFT));

			wxString scope_names[] = {_("Selected lines"), _("All lines"), _("Active line")};
			scope_choice = new wxChoice(this, -1, wxDefaultPosition, wxDefaultSize, 3, scope_names);
			scope_choice->SetSelection(OPT_GET("AI/Scope")->GetInt() < 3 ? OPT_GET("AI/Scope")->GetInt() : 0);

			wxArrayString action_names;
			for (auto const& a : actions) action_names.Add(a.label);
			action_choice = new wxChoice(this, -1, wxDefaultPosition, wxDefaultSize, action_names);
			action_choice->SetSelection(0);

			grid->Add(new wxStaticText(this, -1, _("Provider:")), wxSizerFlags().Center().Left());
			grid->Add(provider_choice, wxSizerFlags().Expand());
			grid->Add(new wxStaticText(this, -1, _("Model:")), wxSizerFlags().Center().Left());
			grid->Add(model_sizer, wxSizerFlags().Expand());
			grid->Add(new wxStaticText(this, -1, _("Apply to:")), wxSizerFlags().Center().Left());
			grid->Add(scope_choice, wxSizerFlags().Expand());
			grid->Add(new wxStaticText(this, -1, _("Task:")), wxSizerFlags().Center().Left());
			grid->Add(action_choice, wxSizerFlags().Expand());

			instruction = new wxTextCtrl(this, -1, to_wx(actions[0].prompt), wxDefaultPosition, FromDIP(wxSize(-1, 90)), wxTE_MULTILINE);

			run_button = new wxButton(this, -1, _("Run"));
			status = new wxStaticText(this, -1, "");

			auto button_sizer = new wxBoxSizer(wxHORIZONTAL);
			button_sizer->Add(status, wxSizerFlags(1).Center().Border(wxRIGHT));
			button_sizer->Add(run_button, wxSizerFlags().Border(wxRIGHT));
			cancel_button = new wxButton(this, wxID_CANCEL, _("Close"));
			button_sizer->Add(cancel_button);

			main_sizer->Add(grid, wxSizerFlags().Expand().Border());
			main_sizer->Add(new wxStaticText(this, -1, _("Instruction:")), wxSizerFlags().Border(wxLEFT | wxRIGHT));
			main_sizer->Add(instruction, wxSizerFlags(1).Expand().Border());
			main_sizer->Add(button_sizer, wxSizerFlags().Expand().Border());
			SetSizerAndFit(main_sizer);

			provider_choice->Bind(wxEVT_CHOICE, &DialogAiAssistant::OnProviderChange, this);
			model_combo->Bind(wxEVT_TEXT, &DialogAiAssistant::OnModelChange, this);
			model_combo->Bind(wxEVT_COMBOBOX, &DialogAiAssistant::OnModelChange, this);
			action_choice->Bind(wxEVT_CHOICE, &DialogAiAssistant::OnActionChange, this);
			refresh_button->Bind(wxEVT_BUTTON, &DialogAiAssistant::OnRefresh, this);
			run_button->Bind(wxEVT_BUTTON, &DialogAiAssistant::OnRun, this);
			Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent& evt) {
				if (busy) evt.Veto();
				else evt.Skip();
			});
			// Block closing (button, [X] and Esc) while a request is running, so
			// the async callbacks never fire into a destroyed dialog.
			Bind(wxEVT_BUTTON, [this](wxCommandEvent& evt) {
				if (!busy) evt.Skip();
			}, wxID_CANCEL);
		}
	};
}

void ShowAiAssistantDialog(agi::Context *c) {
	DialogAiAssistant(c).ShowModal();
}
