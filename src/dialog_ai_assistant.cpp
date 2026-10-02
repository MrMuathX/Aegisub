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

#include <atomic>
#include <string>
#include <utility>
#include <vector>

#include <wx/button.h>
#include <wx/choice.h>
#include <wx/combobox.h>
#include <wx/dialog.h>
#include <wx/gauge.h>
#include <wx/listctrl.h>
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

	/// One processed line, as shown in the results table.
	struct LineResult {
		enum class State { Pending, Unchanged, Applied, Reverted };
		AssDialogue *line;
		std::string before;
		std::string after;
		State state;
	};

	class DialogAiAssistant final : public wxDialog {
		agi::Context *c;

		wxChoice *provider_choice;
		wxStaticText *key_label;
		wxTextCtrl *key_text;
		wxComboBox *model_combo;
		wxButton *refresh_button;
		wxChoice *scope_choice;
		wxChoice *action_choice;
		wxTextCtrl *instruction;
		wxButton *run_button;
		wxButton *stop_button;
		wxGauge *progress;
		wxButton *cancel_button;
		wxStaticText *status;
		wxListCtrl *results_list;
		wxButton *revert_selected_button;
		wxButton *revert_all_button;

		/// Rows of results_list, in the same order. Only touched on the main thread.
		std::vector<LineResult> results;

		bool busy = false;
		/// Raised by Stop; read by the background worker and the HTTP client.
		std::atomic<bool> cancel_requested{false};

		ai::Config MakeConfig() {
			ai::Config cfg;
			cfg.provider = OPT_GET("AI/Provider")->GetString();
			cfg.endpoint = OPT_GET("AI/Endpoints/" + cfg.provider)->GetString();
			if (cfg.provider != "Ollama")
				cfg.key = trim(OPT_GET("AI/Keys/" + cfg.provider)->GetString());
			cfg.model = from_wx(model_combo->GetValue());
			cfg.cancel = &cancel_requested;
			return cfg;
		}

		/// Returns false and shows a message if the provider needs a key and none is set.
		bool CheckKey(ai::Config const& cfg) {
			if (cfg.provider == "Ollama" || !cfg.key.empty()) return true;
			status->SetLabel(wxString::Format(_("Enter an API key for %s first."), to_wx(cfg.provider)));
			key_text->SetFocus();
			return false;
		}

		void LoadKey() {
			std::string provider = OPT_GET("AI/Provider")->GetString();
			bool needs_key = provider != "Ollama";
			key_text->ChangeValue(needs_key ? to_wx(OPT_GET("AI/Keys/" + provider)->GetString()) : wxString());
			key_text->Enable(needs_key && !busy);
			key_label->Enable(needs_key);
		}

		void OnKeyChange(wxCommandEvent&) {
			std::string provider = OPT_GET("AI/Provider")->GetString();
			if (provider != "Ollama")
				OPT_SET("AI/Keys/" + provider)->SetString(trim(from_wx(key_text->GetValue())));
		}

		void SetBusy(bool b) {
			busy = b;
			provider_choice->Enable(!b);
			key_text->Enable(!b && OPT_GET("AI/Provider")->GetString() != "Ollama");
			model_combo->Enable(!b);
			refresh_button->Enable(!b);
			scope_choice->Enable(!b);
			action_choice->Enable(!b);
			instruction->Enable(!b);
			run_button->Enable(!b);
			cancel_button->Enable(!b);
			stop_button->Enable(b);
			UpdateRevertButtons();
			if (b) {
				cancel_requested = false;
				progress->SetValue(0);
			}
		}

		static wxString StateLabel(LineResult::State st) {
			switch (st) {
				case LineResult::State::Pending:   return _("Pending");
				case LineResult::State::Unchanged: return _("Unchanged");
				case LineResult::State::Applied:   return _("Applied");
				case LineResult::State::Reverted:  return _("Reverted");
			}
			return "";
		}

		void AddResultRow(LineResult r) {
			long row = results_list->GetItemCount();
			results_list->InsertItem(row, wxString::Format("%d", r.line->Row + 1));
			results_list->SetItem(row, 1, to_wx(r.before));
			results_list->SetItem(row, 2, to_wx(r.after));
			results_list->SetItem(row, 3, StateLabel(r.state));
			results_list->EnsureVisible(row);
			results.push_back(std::move(r));
		}

		void SetRowState(size_t row, LineResult::State st) {
			results[row].state = st;
			results_list->SetItem((long)row, 3, StateLabel(st));
		}

		void UpdateRevertButtons() {
			bool any_applied = false;
			for (auto const& r : results)
				any_applied |= r.state == LineResult::State::Applied;
			revert_all_button->Enable(!busy && any_applied);
			revert_selected_button->Enable(!busy && any_applied && results_list->GetSelectedItemCount() > 0);
		}

		/// Restore the original text of the given rows that are currently applied.
		void Revert(std::vector<size_t> const& rows) {
			int count = 0;
			for (size_t row : rows) {
				if (results[row].state != LineResult::State::Applied) continue;
				results[row].line->Text = results[row].before;
				SetRowState(row, LineResult::State::Reverted);
				++count;
			}
			if (count) {
				c->ass->Commit(_("AI revert"), AssFile::COMMIT_DIAG_TEXT);
				status->SetLabel(wxString::Format(_("Reverted %d line(s)."), count));
			}
			UpdateRevertButtons();
		}

		void OnRevertSelected(wxCommandEvent&) {
			if (busy) return;
			std::vector<size_t> rows;
			for (long i = results_list->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED); i != -1;
			     i = results_list->GetNextItem(i, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED))
				rows.push_back((size_t)i);
			Revert(rows);
		}

		void OnRevertAll(wxCommandEvent&) {
			if (busy) return;
			std::vector<size_t> rows(results.size());
			for (size_t i = 0; i < rows.size(); ++i) rows[i] = i;
			Revert(rows);
		}

		void OnStop(wxCommandEvent&) {
			if (!busy || cancel_requested) return;
			cancel_requested = true;
			stop_button->Enable(false);
			status->SetLabel(_("Stopping..."));
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
			LoadKey();
		}

		void OnModelChange(wxCommandEvent&) {
			OPT_SET("AI/Model")->SetString(from_wx(model_combo->GetValue()));
		}

		void OnActionChange(wxCommandEvent&) {
			instruction->SetValue(to_wx(actions[action_choice->GetSelection()].prompt));
		}

		void OnRefresh(wxCommandEvent&) {
			if (busy) return;
			ai::Config cfg = MakeConfig();
			if (!CheckKey(cfg)) return;
			SetBusy(true);
			progress->Pulse();
			status->SetLabel(_("Fetching models..."));
			agi::dispatch::Background().Async([=, this] {
				std::vector<std::string> models;
				std::string error;
				bool cancelled = false;
				try {
					models = ai::ListModels(cfg);
				}
				catch (ai::Cancelled const&) {
					cancelled = true;
				}
				catch (std::exception const& e) {
					error = e.what();
				}
				agi::dispatch::Main().Async([=, this] {
					progress->SetValue(0);
					if (cancelled) {
						status->SetLabel(_("Stopped."));
					}
					else if (!error.empty()) {
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
			if (!CheckKey(cfg)) return;

			std::string sys = from_wx(instruction->GetValue());
			sys += system_suffix;

			const int total = (int)lines.size();
			results.clear();
			results_list->DeleteAllItems();
			SetBusy(true);
			progress->SetRange(total);
			status->SetLabel(wxString::Format(_("Processing line 1 of %d..."), total));

			agi::dispatch::Background().Async([=, this] {
				std::string error;
				bool cancelled = false;
				int done = 0;
				try {
					for (auto line : lines) {
						if (cancel_requested) {
							cancelled = true;
							break;
						}
						std::string in = line->Text.get();
						std::string out = trim(ai::Chat(cfg, sys, in));
						bool changed = !out.empty() && out != in;
						++done;
						agi::dispatch::Main().Async([=, this] {
							AddResultRow({line, in, changed ? out : in,
								changed ? LineResult::State::Pending : LineResult::State::Unchanged});
							progress->SetValue(done);
							if (done < total && !cancel_requested)
								status->SetLabel(wxString::Format(_("Processing line %d of %d..."), done + 1, total));
						});
					}
				}
				catch (ai::Cancelled const&) {
					cancelled = true;
				}
				catch (std::exception const& e) {
					error = e.what();
				}
				agi::dispatch::Main().Async([=, this] {
					// Apply every line that finished, even after a stop or error,
					// as one undo step. Each can still be reverted from the table.
					int updated = 0;
					for (size_t i = 0; i < results.size(); ++i) {
						if (results[i].state != LineResult::State::Pending) continue;
						results[i].line->Text = results[i].after;
						SetRowState(i, LineResult::State::Applied);
						++updated;
					}
					if (updated)
						c->ass->Commit(_("AI edit"), AssFile::COMMIT_DIAG_TEXT);

					if (cancelled)
						status->SetLabel(wxString::Format(_("Stopped after %d of %d line(s); %d updated."), done, total, updated));
					else if (!error.empty())
						status->SetLabel(wxString::Format(_("Error after %d of %d line(s); %d updated: %s"), done, total, updated, to_wx(error)));
					else
						status->SetLabel(wxString::Format(_("Updated %d of %d line(s)."), updated, total));
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

			key_label = new wxStaticText(this, -1, _("API key:"));
			key_text = new wxTextCtrl(this, -1, "", wxDefaultPosition, wxDefaultSize, wxTE_PASSWORD);
			key_text->SetHint(_("Paste the provider's API key"));

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
			grid->Add(key_label, wxSizerFlags().Center().Left());
			grid->Add(key_text, wxSizerFlags().Expand());
			grid->Add(new wxStaticText(this, -1, _("Model:")), wxSizerFlags().Center().Left());
			grid->Add(model_sizer, wxSizerFlags().Expand());
			grid->Add(new wxStaticText(this, -1, _("Apply to:")), wxSizerFlags().Center().Left());
			grid->Add(scope_choice, wxSizerFlags().Expand());
			grid->Add(new wxStaticText(this, -1, _("Task:")), wxSizerFlags().Center().Left());
			grid->Add(action_choice, wxSizerFlags().Expand());

			instruction = new wxTextCtrl(this, -1, to_wx(actions[0].prompt), wxDefaultPosition, FromDIP(wxSize(-1, 90)), wxTE_MULTILINE);

			run_button = new wxButton(this, -1, _("Run"));
			stop_button = new wxButton(this, -1, _("Stop"));
			stop_button->Enable(false);
			status = new wxStaticText(this, -1, "");
			progress = new wxGauge(this, -1, 1, wxDefaultPosition, FromDIP(wxSize(-1, 12)));

			results_list = new wxListCtrl(this, -1, wxDefaultPosition, FromDIP(wxSize(720, 200)), wxLC_REPORT);
			results_list->AppendColumn(_("#"), wxLIST_FORMAT_RIGHT, FromDIP(45));
			results_list->AppendColumn(_("Before"), wxLIST_FORMAT_LEFT, FromDIP(280));
			results_list->AppendColumn(_("After"), wxLIST_FORMAT_LEFT, FromDIP(280));
			results_list->AppendColumn(_("Status"), wxLIST_FORMAT_LEFT, FromDIP(90));
			revert_selected_button = new wxButton(this, -1, _("Revert selected"));
			revert_all_button = new wxButton(this, -1, _("Revert all"));
			revert_selected_button->Enable(false);
			revert_all_button->Enable(false);
			auto revert_sizer = new wxBoxSizer(wxHORIZONTAL);
			revert_sizer->AddStretchSpacer();
			revert_sizer->Add(revert_selected_button, wxSizerFlags().Border(wxRIGHT));
			revert_sizer->Add(revert_all_button);

			auto button_sizer = new wxBoxSizer(wxHORIZONTAL);
			button_sizer->Add(status, wxSizerFlags(1).Center().Border(wxRIGHT));
			button_sizer->Add(run_button, wxSizerFlags().Border(wxRIGHT));
			button_sizer->Add(stop_button, wxSizerFlags().Border(wxRIGHT));
			cancel_button = new wxButton(this, wxID_CANCEL, _("Close"));
			button_sizer->Add(cancel_button);

			main_sizer->Add(grid, wxSizerFlags().Expand().Border());
			main_sizer->Add(new wxStaticText(this, -1, _("Instruction:")), wxSizerFlags().Border(wxLEFT | wxRIGHT));
			main_sizer->Add(instruction, wxSizerFlags(1).Expand().Border());
			main_sizer->Add(new wxStaticText(this, -1, _("Results:")), wxSizerFlags().Border(wxLEFT | wxRIGHT));
			main_sizer->Add(results_list, wxSizerFlags(2).Expand().Border());
			main_sizer->Add(revert_sizer, wxSizerFlags().Expand().Border(wxLEFT | wxRIGHT | wxBOTTOM));
			main_sizer->Add(progress, wxSizerFlags().Expand().Border(wxLEFT | wxRIGHT));
			main_sizer->Add(button_sizer, wxSizerFlags().Expand().Border());
			SetSizerAndFit(main_sizer);
			LoadKey();

			provider_choice->Bind(wxEVT_CHOICE, &DialogAiAssistant::OnProviderChange, this);
			key_text->Bind(wxEVT_TEXT, &DialogAiAssistant::OnKeyChange, this);
			model_combo->Bind(wxEVT_TEXT, &DialogAiAssistant::OnModelChange, this);
			model_combo->Bind(wxEVT_COMBOBOX, &DialogAiAssistant::OnModelChange, this);
			action_choice->Bind(wxEVT_CHOICE, &DialogAiAssistant::OnActionChange, this);
			refresh_button->Bind(wxEVT_BUTTON, &DialogAiAssistant::OnRefresh, this);
			run_button->Bind(wxEVT_BUTTON, &DialogAiAssistant::OnRun, this);
			stop_button->Bind(wxEVT_BUTTON, &DialogAiAssistant::OnStop, this);
			revert_selected_button->Bind(wxEVT_BUTTON, &DialogAiAssistant::OnRevertSelected, this);
			revert_all_button->Bind(wxEVT_BUTTON, &DialogAiAssistant::OnRevertAll, this);
			results_list->Bind(wxEVT_LIST_ITEM_SELECTED, [this](wxListEvent&) { UpdateRevertButtons(); });
			results_list->Bind(wxEVT_LIST_ITEM_DESELECTED, [this](wxListEvent&) { UpdateRevertButtons(); });
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
