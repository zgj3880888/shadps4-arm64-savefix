// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>

#include <imgui.h>
#include <magic_enum/magic_enum.hpp>

#include "common/assert.h"
#include "common/logging/log.h"
#include "common/path_util.h"
#include "core/libraries/libs.h"
#include "core/libraries/system/msgdialog.h"
#include "imgui_internal.h"
#include "msgdialog_ui.h"

namespace Libraries::MsgDialog {

using CommonDialog::Error;
using CommonDialog::Result;
using CommonDialog::Status;

static auto g_status = Status::NONE;
static MsgDialogState g_state{};
static DialogResult g_result{};
static MsgDialogUi g_msg_dialog_ui;

namespace {

// The Android frontend only forwards real finger touches to the guest, so an ImGui modal that the
// game blocks on never receives its answer and the title screen hangs forever. When the marker file
// exists the dialog is answered immediately and the game continues on its offline path. Put "yes"
// in the file to pick the first (Yes) button instead of No/Cancel.
std::optional<ButtonId> AutoAnswerButton(const OrbisParam* param) {
    if (param->mode == MsgDialogMode::PROGRESS_BAR) {
        return std::nullopt; // driven by the guest, never by the user
    }

    std::error_code ec;
    const auto flag =
        Common::FS::GetUserPath(Common::FS::PathType::UserDir) / "msgdialog_autoconfirm";
    if (!std::filesystem::exists(flag, ec)) {
        return std::nullopt;
    }

    std::string body;
    {
        std::ifstream f(flag, std::ios::binary);
        body.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
        for (auto& c : body) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
    }
    const bool choose_yes = body.find("yes") != std::string::npos;

    ButtonId button = choose_yes ? ButtonId::BUTTON1 : ButtonId::BUTTON2;
    if (param->mode == MsgDialogMode::SYSTEM_MSG) {
        button = ButtonId::OK; // single OK button
    } else if (param->userMsgParam != nullptr) {
        switch (param->userMsgParam->buttonType) {
        case ButtonType::OK:
            button = ButtonId::OK;
            break;
        case ButtonType::NONE:
            button = ButtonId::INVALID;
            break;
        case ButtonType::WAIT:
            return std::nullopt; // spinner: closing it would desync the guest
        case ButtonType::OK_CANCEL:
        case ButtonType::OK_CANCEL_FOCUS_CANCEL:
        case ButtonType::WAIT_CANCEL:
            button = ButtonId::INVALID;
            break;
        default:
            break;
        }
    }

    if (param->userMsgParam != nullptr) {
        LOG_INFO(Lib_MsgDlg, "autoconfirm: mode={} buttonType={} -> button_id={}",
                 magic_enum::enum_name(param->mode),
                 magic_enum::enum_name(param->userMsgParam->buttonType),
                 static_cast<u32>(button));
    } else {
        LOG_INFO(Lib_MsgDlg, "autoconfirm: mode={} -> button_id={}",
                 magic_enum::enum_name(param->mode), static_cast<u32>(button));
    }
    return button;
}

} // namespace

Error PS4_SYSV_ABI sceMsgDialogClose() {
    LOG_DEBUG(Lib_MsgDlg, "called");
    if (g_status != Status::RUNNING) {
        return Error::NOT_RUNNING;
    }
    g_msg_dialog_ui.Finish(ButtonId::INVALID);
    return Error::OK;
}

Error PS4_SYSV_ABI sceMsgDialogGetResult(DialogResult* result) {
    LOG_DEBUG(Lib_MsgDlg, "called");
    if (g_status != Status::FINISHED) {
        return Error::NOT_FINISHED;
    }
    if (result == nullptr) {
        return Error::ARG_NULL;
    }
    *result = g_result;
    return Error::OK;
}

Status PS4_SYSV_ABI sceMsgDialogGetStatus() {
    LOG_TRACE(Lib_MsgDlg, "called status={}", magic_enum::enum_name(g_status));
    return g_status;
}

Error PS4_SYSV_ABI sceMsgDialogInitialize() {
    LOG_DEBUG(Lib_MsgDlg, "called");
    if (!CommonDialog::g_isInitialized) {
        return Error::NOT_SYSTEM_INITIALIZED;
    }
    if (g_status != Status::NONE) {
        return Error::ALREADY_INITIALIZED;
    }
    if (CommonDialog::g_isUsed) {
        return Error::BUSY;
    }
    g_status = Status::INITIALIZED;
    CommonDialog::g_isUsed = true;

    return Error::OK;
}

Error PS4_SYSV_ABI sceMsgDialogOpen(const OrbisParam* param) {
    if (g_status != Status::INITIALIZED && g_status != Status::FINISHED) {
        LOG_INFO(Lib_MsgDlg, "called without initialize");
        return Error::INVALID_STATE;
    }
    if (param == nullptr) {
        LOG_DEBUG(Lib_MsgDlg, "called param:(NULL)");
        return Error::ARG_NULL;
    }
    LOG_DEBUG(Lib_MsgDlg, "called param->mode: {}", magic_enum::enum_name(param->mode));
    ASSERT(param->size == sizeof(OrbisParam));
    ASSERT(param->baseParam.size == sizeof(CommonDialog::BaseParam));
    g_result = {};
    g_state = MsgDialogState{*param};
    g_status = Status::RUNNING;
    g_msg_dialog_ui = MsgDialogUi(&g_state, &g_status, &g_result);

    // Diagnostic: the guest strings are Shift-JIS bytes and the UI font lacks the glyphs, so the
    // on-screen text is unreadable. Logging the raw bytes identifies the prompt from the log alone.
    if (param->mode == MsgDialogMode::USER_MSG && param->userMsgParam != nullptr &&
        param->userMsgParam->msg != nullptr) {
        char hex[194]{};
        std::size_t n = 0;
        for (; n < 96 && param->userMsgParam->msg[n] != '\0'; ++n) {
            std::snprintf(hex + n * 2, 3, "%02x",
                          static_cast<unsigned char>(param->userMsgParam->msg[n]));
        }
        LOG_INFO(Lib_MsgDlg, "user msg: buttonType={} hex={}",
                 magic_enum::enum_name(param->userMsgParam->buttonType), hex);
    }

    if (const auto button = AutoAnswerButton(param)) {
        g_msg_dialog_ui.Finish(*button);
    }

    return Error::OK;
}

Error PS4_SYSV_ABI sceMsgDialogProgressBarInc(OrbisMsgDialogProgressBarTarget target, u32 delta) {
    LOG_DEBUG(Lib_MsgDlg, "called");
    if (g_status != Status::RUNNING) {
        return Error::NOT_RUNNING;
    }
    if (g_state.GetMode() != MsgDialogMode::PROGRESS_BAR) {
        return Error::NOT_SUPPORTED;
    }
    if (target != OrbisMsgDialogProgressBarTarget::DEFAULT) {
        return Error::PARAM_INVALID;
    }
    g_state.GetState<MsgDialogState::ProgressState>().progress += delta;
    return Error::OK;
}

Error PS4_SYSV_ABI sceMsgDialogProgressBarSetMsg(OrbisMsgDialogProgressBarTarget target,
                                                 const char* msg) {
    LOG_DEBUG(Lib_MsgDlg, "called");
    if (g_status != Status::RUNNING) {
        return Error::NOT_RUNNING;
    }
    if (g_state.GetMode() != MsgDialogMode::PROGRESS_BAR) {
        return Error::NOT_SUPPORTED;
    }
    if (target != OrbisMsgDialogProgressBarTarget::DEFAULT) {
        return Error::PARAM_INVALID;
    }
    g_state.GetState<MsgDialogState::ProgressState>().msg = msg;
    return Error::OK;
}

Error PS4_SYSV_ABI sceMsgDialogProgressBarSetValue(OrbisMsgDialogProgressBarTarget target,
                                                   u32 value) {
    LOG_DEBUG(Lib_MsgDlg, "called");
    if (g_status != Status::RUNNING) {
        return Error::NOT_RUNNING;
    }
    if (g_state.GetMode() != MsgDialogMode::PROGRESS_BAR) {
        return Error::NOT_SUPPORTED;
    }
    if (target != OrbisMsgDialogProgressBarTarget::DEFAULT) {
        return Error::PARAM_INVALID;
    }
    g_state.GetState<MsgDialogState::ProgressState>().progress = value;
    return Error::OK;
}

Error PS4_SYSV_ABI sceMsgDialogTerminate() {
    LOG_DEBUG(Lib_MsgDlg, "called");
    if (g_status == Status::RUNNING) {
        sceMsgDialogClose();
    }
    if (g_status == Status::NONE) {
        return Error::NOT_INITIALIZED;
    }
    g_status = Status::NONE;
    CommonDialog::g_isUsed = false;
    return Error::OK;
}

Status PS4_SYSV_ABI sceMsgDialogUpdateStatus() {
    LOG_TRACE(Lib_MsgDlg, "called status={}", magic_enum::enum_name(g_status));
    return g_status;
}

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("HTrcDKlFKuM", "libSceMsgDialog", 1, "libSceMsgDialog", sceMsgDialogClose);
    LIB_FUNCTION("Lr8ovHH9l6A", "libSceMsgDialog", 1, "libSceMsgDialog", sceMsgDialogGetResult);
    LIB_FUNCTION("CWVW78Qc3fI", "libSceMsgDialog", 1, "libSceMsgDialog", sceMsgDialogGetStatus);
    LIB_FUNCTION("lDqxaY1UbEo", "libSceMsgDialog", 1, "libSceMsgDialog", sceMsgDialogInitialize);
    LIB_FUNCTION("b06Hh0DPEaE", "libSceMsgDialog", 1, "libSceMsgDialog", sceMsgDialogOpen);
    LIB_FUNCTION("Gc5k1qcK4fs", "libSceMsgDialog", 1, "libSceMsgDialog",
                 sceMsgDialogProgressBarInc);
    LIB_FUNCTION("6H-71OdrpXM", "libSceMsgDialog", 1, "libSceMsgDialog",
                 sceMsgDialogProgressBarSetMsg);
    LIB_FUNCTION("wTpfglkmv34", "libSceMsgDialog", 1, "libSceMsgDialog",
                 sceMsgDialogProgressBarSetValue);
    LIB_FUNCTION("ePw-kqZmelo", "libSceMsgDialog", 1, "libSceMsgDialog", sceMsgDialogTerminate);
    LIB_FUNCTION("6fIC3XKt2k0", "libSceMsgDialog", 1, "libSceMsgDialog", sceMsgDialogUpdateStatus);
};

} // namespace Libraries::MsgDialog
