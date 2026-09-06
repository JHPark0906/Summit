#include "ChatTestSupport.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>

#include "Runtime/LayoutElement.h"
#include "Runtime/ScrollRect.h"
#include "Runtime/TextRenderer.h"
#include "Runtime/Transform.h"

namespace
{
using ChatTestSupport::Fixture;
using ChatTestSupport::Require;
namespace Runtime = GameEngine::Runtime;
namespace Platform = GameEngine::Platform;

void KeyboardAndButtonSubmission(const unsigned int scale)
{
    Fixture fixture(scale);
    auto& field = fixture.Component<Runtime::InputField>("ChatInput");
    Require(!fixture.chat->IsEditing() && !field.IsFocused(), "chat began with keyboard focus");
    fixture.Enter();
    Require(fixture.chat->IsEditing() && field.IsFocused() && !fixture.chat->ConsumeSubmittedMessage(),
        "Enter must open chat without immediately submitting or losing keyboard focus");
    Platform::InputState composition;
    composition.compositionText = "한";
    composition.SetKey(Platform::Key::Enter, true);
    fixture.Step(composition);
    fixture.Step();
    Require(fixture.chat->IsEditing() && !fixture.chat->ConsumeSubmittedMessage(),
        "Enter used for an IME composition must not submit chat");
    Platform::InputState typing;
    typing.typedText = "한글 채팅 테스트";
    fixture.Step(typing);
    Require(field.GetText() == typing.typedText, "Korean typing did not reach the requested chat focus");
    fixture.Enter();
    const auto message = fixture.chat->ConsumeSubmittedMessage();
    Require(message && *message == typing.typedText && !fixture.chat->IsEditing() && !field.IsFocused(),
        "Enter did not submit the exact committed message and release editing");
    Require(!fixture.chat->ConsumeSubmittedMessage(), "chat submission was delivered more than once");

    // Reading history must not leave an open composer without its keyboard recipient.
    fixture.Enter();
    field.SetText("기록을 확인한 뒤 엔터 전송");
    fixture.Click("ChatViewport");
    fixture.Enter();
    const auto afterHistoryClick = fixture.chat->ConsumeSubmittedMessage();
    Require(afterHistoryClick && *afterHistoryClick == "기록을 확인한 뒤 엔터 전송" &&
        !fixture.chat->IsEditing(), "clicking chat history swallowed the next Enter submission");

    fixture.Enter();
    field.SetText("엔터 후 다음 프레임에 게임 클릭");
    Platform::InputState acceptedEnter;
    acceptedEnter.SetKey(Platform::Key::Enter, true);
    acceptedEnter.SetKey(Platform::Key::Enter, false);
    fixture.Step(acceptedEnter);
    Platform::InputState outside;
    outside.cursor = { static_cast<int>(fixture.width - 1), 1 };
    outside.SetMouseButton(Platform::MouseButton::Left, true);
    fixture.Step(outside);
    const auto beforeCancel = fixture.chat->ConsumeSubmittedMessage();
    Require(beforeCancel && *beforeCancel == "엔터 후 다음 프레임에 게임 클릭",
        "a later world click cancelled an Enter submission that the InputField had already accepted");
    fixture.Step();

    fixture.Enter();
    field.SetText("확정 ");
    Platform::InputState committedIme;
    committedIme.typedText = "한";
    committedIme.SetKey(Platform::Key::Enter, true);
    committedIme.MarkKeyHandledByIme(Platform::Key::Enter);
    fixture.Step(committedIme);
    fixture.Step();
    Require(fixture.chat->IsEditing() && field.GetText() == "확정 한" && !fixture.chat->ConsumeSubmittedMessage(),
        "unidentified IME text must not fabricate a chat submission");
    fixture.Enter();
    const auto afterIme = fixture.chat->ConsumeSubmittedMessage();
    Require(afterIme && *afterIme == "확정 한", "ordinary Enter after IME confirmation did not submit exactly once");

    fixture.Click("ChatOpenButton");
    Require(fixture.chat->IsEditing() && field.IsFocused(), "chat open button did not request input focus");
    field.SetText("버튼 전송");
    fixture.Click("ChatSendButton");
    const auto clicked = fixture.chat->ConsumeSubmittedMessage();
    Require(clicked && *clicked == "버튼 전송", "send button did not submit the current draft");

    fixture.Enter();
    field.SetText("취소할 내용");
    Platform::InputState escape;
    escape.SetKey(Platform::Key::Escape, true);
    fixture.Step(escape);
    Require(!fixture.chat->IsEditing() && fixture.chat->WasInputConsumedThisFrame() &&
        field.GetText().empty() && !fixture.chat->ConsumeSubmittedMessage(),
        "Escape must consume the key and cancel the draft without sending it");
    fixture.Step();
    fixture.Enter();
    fixture.chat->SetAvailable(false);
    fixture.Step();
    Require(!fixture.chat->IsEditing() && !field.IsFocused() &&
        !fixture.scene->FindGameObject("ChatPanel")->IsActive(),
        "profile/network unavailability must hide chat and release its keyboard focus");
    fixture.Enter();
    Require(!fixture.chat->IsEditing(), "unavailable chat took Enter away from profile UI");
}

void ImeCommitSubmission(const unsigned int scale)
{
    Fixture fixture(scale);
    auto& field = fixture.Component<Runtime::InputField>("ChatInput");
    auto enter = [](const std::uint64_t id)
    {
        Platform::InputState state;
        state.compositionId = id;
        state.imeEnterCompositionId = id;
        state.MarkKeyHandledByIme(Platform::Key::Enter);
        state.SetKey(Platform::Key::Enter, true);
        return state;
    };
    fixture.Enter();
    field.SetText("확정 ");
    auto sameFrame = enter(1);
    sameFrame.committedCompositionId = 1;
    sameFrame.typedText = "한";
    fixture.Step(sameFrame);
    Require(field.GetText() == "확정 한" && !fixture.chat->ConsumeSubmittedMessage(),
        "IME Enter sent before the InputField applied its same-frame Korean result");
    fixture.Step();
    auto sent = fixture.chat->ConsumeSubmittedMessage();
    Require(sent && *sent == "확정 한" && !fixture.chat->IsEditing(),
        "one IME Enter must commit and submit the final Korean character exactly once");
    sameFrame.TakeAccumulated();
    fixture.Step(sameFrame);
    fixture.Step(sameFrame);
    Require(!fixture.chat->ConsumeSubmittedMessage() && !fixture.chat->IsEditing(),
        "holding Enter reopened chat or delivered the IME submission twice");
    fixture.Step();

    fixture.Enter();
    field.SetText("지연 ");
    auto pending = enter(2);
    pending.compositionText = "글";
    fixture.Step(pending);
    Platform::InputState ended;
    ended.compositionId = 2;
    fixture.Step(ended);
    Require(fixture.chat->IsEditing() && !fixture.chat->ConsumeSubmittedMessage(),
        "END before RESULT submitted a draft without its final Korean character");
    auto result = ended;
    result.committedCompositionId = 2;
    result.typedText = "글";
    fixture.Step(result);
    Require(!fixture.chat->ConsumeSubmittedMessage(), "delayed IME result bypassed the field editing pass");
    fixture.Step();
    sent = fixture.chat->ConsumeSubmittedMessage();
    Require(sent && *sent == "지연 글", "delayed RESULT after END did not complete the original Enter submission");

    fixture.Enter();
    field.SetText("취소 뒤 ");
    fixture.Step(enter(3));
    ended.compositionId = 3;
    fixture.Step(ended);
    result.compositionId = 4;
    result.committedCompositionId = 4;
    result.typedText = "새";
    fixture.Step(result);
    fixture.Step();
    Require(fixture.chat->IsEditing() && field.GetText() == "취소 뒤 새" && !fixture.chat->ConsumeSubmittedMessage(),
        "a new composition's result revived a cancelled composition's pending Enter");

    fixture.Step(enter(5));
    Platform::InputState escape;
    escape.compositionId = 5;
    escape.SetKey(Platform::Key::Escape, true);
    escape.MarkKeyHandledByIme(Platform::Key::Escape);
    fixture.Step(escape);
    result.compositionId = 5;
    result.committedCompositionId = 5;
    fixture.Step(result);
    fixture.Step();
    Require(fixture.chat->IsEditing() && !fixture.chat->ConsumeSubmittedMessage(),
        "IME-handled Escape failed to cancel a pending submission");

    fixture.Step(enter(6));
    Platform::InputState unfocused;
    fixture.game.GetInput().BeginFrameWithState(unfocused);
    fixture.chat->TickInput();
    fixture.game.Update(0);
    Require(fixture.chat->IsEditing() && !field.GetText().empty(),
        "losing window focus discarded the unsent chat draft");
    fixture.Step();
    result.compositionId = 6;
    result.committedCompositionId = 6;
    fixture.Step(result);
    fixture.Step();
    Require(fixture.chat->IsEditing() && !fixture.chat->ConsumeSubmittedMessage(),
        "a late result revived the submission cancelled by window focus loss");
    Require(field.IsFocused(), "returning to the window did not restore the chat draft's keyboard focus");

    fixture.Step(enter(7));
    fixture.chat->CancelEditing();
    fixture.Step();
    fixture.Enter();
    result.compositionId = 7;
    result.committedCompositionId = 7;
    fixture.Step(result);
    fixture.Step();
    Require(fixture.chat->IsEditing() && !fixture.chat->ConsumeSubmittedMessage(),
        "a late result from the old editing session submitted newly opened chat");
    fixture.game.GetInput().BeginFrameWithState(unfocused);
    fixture.chat->TickInput();
    fixture.game.Update(0);
    field.SetText("복귀와 동시에 Enter");
    fixture.Enter();
    sent = fixture.chat->ConsumeSubmittedMessage();
    Require(sent && *sent == "복귀와 동시에 Enter", "Enter during window focus restoration was swallowed");
}

void BoundsWrappingAndScroll(const unsigned int scale)
{
    Fixture fixture(scale);
    auto& field = fixture.Component<Runtime::InputField>("ChatInput");
    fixture.Enter();
    field.SetText(std::string(513, 'a'));
    fixture.Enter();
    Require(fixture.chat->IsEditing() && field.IsFocused() && field.GetText().size() == 513 &&
        !fixture.chat->ConsumeSubmittedMessage(), "oversized chat must preserve the editable draft and show an error");
    field.SetText("line\nbreak");
    fixture.Enter();
    Require(fixture.chat->IsEditing() && !fixture.chat->ConsumeSubmittedMessage(),
        "control characters must be rejected before losing the draft");
    std::string longest;
    for (int index = 0; index < 170; ++index) longest += "가";
    longest += "ab";
    Require(longest.size() == 512, "UTF-8 message boundary fixture is incorrect");
    field.SetText(longest);
    fixture.Enter();
    const auto message = fixture.chat->ConsumeSubmittedMessage();
    Require(message && *message == longest, "an exact 512-byte Korean message must survive submission unchanged");
    fixture.chat->AddMessage("한글닉네임", longest);
    fixture.Step();
    const auto* row = fixture.scene->FindGameObject("ChatMessage");
    Require(row && row->GetComponent<Runtime::LayoutElement>()->IsWrapping() &&
        row->GetComponent<Runtime::RectTransform>()->GetDesiredSize().height > 80 * scale,
        "the actual Korean font must wrap a long chat message into multiple measured lines");
    auto& viewport = fixture.Component<Runtime::RectTransform>("ChatViewport");
    Require(!row->GetComponent<Runtime::RectTransform>()->GetVisibleRect().IsEmpty(),
        "the newest wrapped message was not scrolled into view");
    for (int index = 0; index < 110; ++index)
        fixture.chat->AddMessage("Player", "message " + std::to_string(index));
    fixture.Step();
    Require(fixture.chat->GetMessageCount() == 100, "chat history exceeded its 100-message bound");
    const auto& children = fixture.scene->FindGameObject("ChatContent")->GetTransform().GetChildren();
    Require(children.size() == 100 &&
        children.front()->GetGameObject()->GetComponent<Runtime::TextRenderer>()->GetText() == "Player: message 10",
        "history bound did not discard the oldest messages and their UI objects");
    auto& scroll = fixture.Component<Runtime::ScrollRect>("ChatViewport");
    auto& content = fixture.Component<Runtime::RectTransform>("ChatContent");
    const float bottom = content.GetDesiredSize().height - viewport.GetResolvedRect().height;
    Require(std::abs(scroll.GetScrollOffset().GetY() - bottom) < 1,
        "new messages must automatically scroll to the latest message");
    Platform::InputState wheel;
    wheel.cursor = { static_cast<int>(viewport.GetResolvedRect().GetCenterX()),
        static_cast<int>(viewport.GetResolvedRect().GetCenterY()) };
    wheel.wheelDelta = 1;
    fixture.Step(wheel);
    const float olderOffset = scroll.GetScrollOffset().GetY();
    Require(std::abs(bottom - olderOffset - 48 * scale) < 1 && fixture.chat->WasInputConsumedThisFrame(),
        "one wheel step must scroll exactly once at the canvas scale and consume that input");
    fixture.Step();
    Require(std::abs(scroll.GetScrollOffset().GetY() - olderOffset) < 1,
        "chat snapped back to the bottom while the user was reading older messages");
    const auto panel = fixture.Component<Runtime::RectTransform>("ChatPanel").GetResolvedRect();
    const auto input = fixture.Component<Runtime::RectTransform>("ChatOpenButton").GetResolvedRect();
    Require(panel.x >= 0 && panel.GetBottom() <= fixture.height && panel.GetRight() <= fixture.width &&
        input.IntersectedWith(viewport.GetResolvedRect()).IsEmpty(),
        "chat history and composer overlap or leave the viewport at this scale");
}

void ProfileAudioControlsFit(const unsigned int scale)
{
    Fixture fixture(scale);
    fixture.chat->SetAvailable(false);
    fixture.scene->FindGameObject("ConnectionCanvas")->SetActive(true);
    fixture.Step();
    const auto panel = fixture.Component<Runtime::RectTransform>("ProfilePanel").GetResolvedRect();
    const std::array names{ "BgmSettingsLabel", "BgmVolumeDownButton", "BgmVolumeText",
        "BgmVolumeUpButton", "BgmMuteButton" };
    Require(panel.y >= 0 && panel.GetBottom() <= fixture.height,
        "the expanded profile panel exceeds the screen at this canvas scale");
    for (std::size_t index = 0; index < names.size(); ++index)
    {
        const auto rect = fixture.Component<Runtime::RectTransform>(names[index]).GetResolvedRect();
        Require(!rect.IsEmpty() && rect.x >= panel.x && rect.GetRight() <= panel.GetRight() &&
            rect.y >= panel.y && rect.GetBottom() <= panel.GetBottom(), "a BGM setting escaped the profile panel");
        for (std::size_t previous = 0; previous < index; ++previous)
            Require(rect.IntersectedWith(fixture.Component<Runtime::RectTransform>(names[previous]).GetResolvedRect()).IsEmpty(),
                "BGM setting controls overlap each other");
        for (const char* other : { "CharacterEButton", "ProfileErrorText", "NicknameSubmitButton", "ProfileCancelButton" })
            Require(rect.IntersectedWith(fixture.Component<Runtime::RectTransform>(other).GetResolvedRect()).IsEmpty(),
                "BGM controls overlap character, validation, or profile submission controls");
    }
}

void PanelResizesWithViewport(const unsigned int scale)
{
    Fixture fixture(scale);
    fixture.Enter();
    for (const auto size : { std::array{ 640u, 360u }, std::array{ 1280u, 720u },
            std::array{ 1920u, 1080u }, std::array{ 960u, 720u } })
    {
        fixture.width = size[0] * scale;
        fixture.height = size[1] * scale;
        fixture.game.SetRenderSurfaceSize(static_cast<float>(fixture.width), static_cast<float>(fixture.height));
        fixture.game.SetRenderAspectRatio(static_cast<float>(fixture.width) / fixture.height);
        fixture.Step();
        const auto panel = fixture.Component<Runtime::RectTransform>("ChatPanel").GetResolvedRect();
        Require(std::abs(panel.width / fixture.width - .39f) < .0001f &&
            std::abs(panel.height / fixture.height - .38f) < .0001f,
            "chat panel is not proportional to the current viewport width and height");
        Require(panel.x >= 0 && panel.y >= 0 && panel.GetRight() <= fixture.width &&
            panel.GetBottom() <= fixture.height, "resizing moved the chat panel outside the viewport");
        for (const char* name : { "ChatViewport", "ChatInputClip", "ChatSendButton", "ChatHint" })
        {
            const auto rect = fixture.Component<Runtime::RectTransform>(name).GetResolvedRect();
            Require(!rect.IsEmpty() && rect.x >= panel.x && rect.y >= panel.y &&
                rect.GetRight() <= panel.GetRight() && rect.GetBottom() <= panel.GetBottom(),
                "chat controls do not fit the resized panel");
        }
        Require(fixture.Component<Runtime::TextRenderer>("ChatInput").GetFontSize() >= 21 &&
            fixture.Component<Runtime::RectTransform>("ChatInputClip").GetResolvedRect().IntersectedWith(
                fixture.Component<Runtime::RectTransform>("ChatViewport").GetResolvedRect()).IsEmpty(),
            "proportional chat shrank its text or overlapped the input and history");
    }
}
}

void RunChatUiTests()
{
    for (const unsigned int scale : { 1u, 2u })
    {
        KeyboardAndButtonSubmission(scale);
        ImeCommitSubmission(scale);
        BoundsWrappingAndScroll(scale);
        ProfileAudioControlsFit(scale);
        PanelResizesWithViewport(scale);
    }
}
