#include "ChatController.h"

#include <algorithm>
#include <cmath>
#include <span>
#include <string_view>
#include <utility>

#include "Math/Color.h"
#include "Platform/IInput.h"
#include "Runtime/Button.h"
#include "Runtime/Canvas.h"
#include "Runtime/GameObject.h"
#include "Runtime/Input.h"
#include "Runtime/InputField.h"
#include "Runtime/LayoutElement.h"
#include "Runtime/PropertyDescriptor.h"
#include "Runtime/RectTransform.h"
#include "Runtime/RuntimeContext.h"
#include "Runtime/Scene.h"
#include "Runtime/ScrollRect.h"
#include "Runtime/TextRenderer.h"
#include "Runtime/Transform.h"

namespace Summit
{
namespace
{
namespace Runtime = GameEngine::Runtime;
namespace Platform = GameEngine::Platform;
namespace Math = GameEngine::Math;

template<class T> T* Find(Runtime::Scene* scene, const char* name)
{
    auto* object = scene ? scene->FindGameObject(name) : nullptr;
    return object ? object->GetComponent<T>() : nullptr;
}

void Activate(Runtime::Scene* scene, const char* name, const bool active)
{
    if (auto* object = scene ? scene->FindGameObject(name) : nullptr) object->SetActive(active);
}

std::string Utf8Prefix(const std::string_view text, const std::size_t maximum)
{
    // 통신 제한은 글자 수가 아닌 UTF-8 바이트 수다. 경계를 줄일 때 다바이트 글자의 중간을 남기지 않는다.
    std::size_t size = (std::min)(text.size(), maximum);
    if (size < text.size())
        while (size > 0 && (static_cast<unsigned char>(text[size]) & 0xc0) == 0x80) --size;
    return std::string(text.substr(0, size));
}

bool IsBlank(const std::string_view text)
{
    return text.find_first_not_of(" \t\r\n") == std::string_view::npos;
}
}

const Runtime::ComponentType& ChatController::StaticType()
{
    static const Runtime::ComponentType type{ "ChatController", &Runtime::MonoBehaviour::StaticType(),
        nullptr, &Runtime::MakeComponentInstance<ChatController> };
    return type;
}

bool ChatController::IsPointerOverUi() const
{
    const auto* context = GetRuntimeContext();
    const auto* rect = Find<Runtime::RectTransform>(GetScene(), "ChatPanel");
    if (!mAvailable || !context || !rect || !rect->GetGameObject()->IsActiveInHierarchy()) return false;
    const auto cursor = context->GetInput().GetMousePosition();
    return rect->GetVisibleRect().Contains(static_cast<float>(cursor.GetX()), static_cast<float>(cursor.GetY()));
}

void ChatController::SetAvailable(const bool available)
{
    mAvailable = available;
    if (!available) CancelEditing();
    RefreshView();
}

void ChatController::RefreshView()
{
    auto* scene = GetScene();
    Activate(scene, "ChatPanel", mAvailable);
    Activate(scene, "ChatOpenButton", !mEditing);
    Activate(scene, "ChatInputClip", mEditing);
    Activate(scene, "ChatSendButton", mEditing);
    if (auto* field = Find<Runtime::InputField>(scene, "ChatInput")) field->SetInteractable(mAvailable && mEditing);
    if (auto* hint = Find<Runtime::TextRenderer>(scene, "ChatHint"))
    {
        if (!mError.empty()) hint->SetText(mError);
        else if (mEditing)
        {
            const auto* context = GetRuntimeContext();
            if (context && !context->GetInput().GetCompositionText().empty())
                hint->SetText("Enter 확정·전송 · Esc 취소");
            else hint->SetText("Enter 전송 · Esc 취소");
        }
        else hint->SetText("Enter 채팅 · 휠로 기록 보기");
        hint->SetColor(mError.empty() ? Math::Color{ .73f, .79f, .87f, 1 } : Math::Color{ 1, .65f, .5f, 1 });
    }
}

void ChatController::BeginEditing()
{
    if (!mAvailable) return;
    mEditing = true;
    mPendingImeComposition = 0;
    mSubmissionReady = false;
    mError.clear();
    mLastDraft.clear();
    RefreshView();
    if (auto* field = Find<Runtime::InputField>(GetScene(), "ChatInput")) field->RequestFocus();
}

void ChatController::CancelEditing()
{
    mEditing = false;
    mPendingImeComposition = 0;
    mSubmissionReady = false;
    mError.clear();
    mLastDraft.clear();
    if (auto* field = Find<Runtime::InputField>(GetScene(), "ChatInput"))
    {
        field->SetInteractable(false);
        field->SetText({});
    }
    RefreshView();
}

std::optional<std::string> ChatController::ConsumeSubmittedMessage()
{
    auto value = std::move(mSubmitted);
    mSubmitted.reset();
    return value;
}

void ChatController::Submit()
{
    auto* field = Find<Runtime::InputField>(GetScene(), "ChatInput");
    if (!field || !mEditing) return;
    const std::string& text = field->GetText();
    if (IsBlank(text)) { CancelEditing(); return; }
    if (text.size() > MaximumMessageBytes)
    {
        mError = "메시지는 512바이트 이내로 입력해 주세요.";
        field->RequestFocus();
        RefreshView();
        return;
    }
    if (std::any_of(text.begin(), text.end(), [](const unsigned char character)
        { return character < 32 || character == 127; }))
    {
        mError = "줄바꿈이나 제어 문자를 제외하고 입력해 주세요.";
        field->RequestFocus();
        RefreshView();
        return;
    }
    if (!mSubmitted) mSubmitted = text;
    CancelEditing();
}

void ChatController::TickInput()
{
    mInputConsumed = false;
    const auto* context = GetRuntimeContext();
    if (!IsActiveAndEnabled() || !context || !mAvailable) return;
    const auto& input = context->GetInput();
    if (!input.GetState().hasFocus)
    {
        mPendingImeComposition = 0;
        mSubmissionReady = false;
        return;
    }
    const bool overUi = IsPointerOverUi();
    const bool mouseEdge = input.GetMouseButtonDown(Platform::MouseButton::Left) ||
        input.GetMouseButtonUp(Platform::MouseButton::Left);
    mInputConsumed = overUi && mouseEdge;
    UpdateScroll();
    if (mEditing)
    {
        auto* field = Find<Runtime::InputField>(GetScene(), "ChatInput");
        if (field && field->GetText() != mLastDraft)
        {
            mLastDraft = field->GetText();
            mError.clear();
        }
        const auto* send = Find<Runtime::Button>(GetScene(), "ChatSendButton");
        // UI 결과는 직전 프레임 후반에 생성된다. 이미 확정된 전송을 이번 프레임의 취소보다 먼저 소비한다.
        // IME Enter는 실제 RESULT의 typedText를 InputField가 적용한 뒤에만 이 경로로 합류한다.
        if (mSubmissionReady || (field && field->WasSubmittedThisFrame()) || (send && send->WasClickedThisFrame()))
        {
            mPendingImeComposition = 0;
            mSubmissionReady = false;
            mInputConsumed = true;
            Submit();
            return;
        }
        if ((input.GetKeyDown(Platform::Key::Escape) && !input.GetState().WasKeyHandledByIme(Platform::Key::Escape)) ||
            (input.GetMouseButtonDown(Platform::MouseButton::Left) && !overUi))
        {
            mInputConsumed = true;
            CancelEditing();
            return;
        }
        // 창 포커스가 돌아오면 초안과 편집 상태를 유지한 채 키보드 수신자를 복구한다.
        // 이때 누른 일반 Enter도 필드가 이번 프레임의 문자를 반영한 뒤 한 번 처리한다.
        if (field && !field->IsFocused())
        {
            field->RequestFocus();
            if (input.GetKeyDown(Platform::Key::Enter) && !input.GetState().imeHandledEnter &&
                !input.GetState().WasKeyHandledByIme(Platform::Key::Enter) && input.GetCompositionText().empty())
                mSubmissionReady = true;
        }
        // 기록이나 패널 여백은 별도 입력 대상이 아니다. 내부 클릭이 편집 중인 필드의 포커스를 빼앗지 않는다.
        // 필드 자체의 클릭은 기존 캐럿/선택 처리를 그대로 거친다.
        if (field && overUi && input.GetMouseButtonDown(Platform::MouseButton::Left))
            if (const auto cursor = input.GetMousePosition();
                !field->Covers(static_cast<float>(cursor.GetX()), static_cast<float>(cursor.GetY())))
                field->RequestFocus();
        if (input.GetKeyDown(Platform::Key::Enter)) mInputConsumed = true;
        const auto& state = input.GetState();
        // IME 내부 Esc 취소도 예약은 버린다. 폼 자체를 닫을지는 위의 일반 Esc 정책이 결정한다.
        if (input.GetKeyDown(Platform::Key::Escape)) mPendingImeComposition = 0;
        if (input.GetKeyDown(Platform::Key::Enter) && state.imeEnterCompositionId != 0)
            mPendingImeComposition = state.imeEnterCompositionId;
        if (mPendingImeComposition != 0)
        {
            if (state.committedCompositionId == mPendingImeComposition)
            {
                mSubmissionReady = true;
                mPendingImeComposition = 0;
            }
            else if (state.compositionId != mPendingImeComposition)
                mPendingImeComposition = 0;
        }
        RefreshView();
    }
    else
    {
        const auto* open = Find<Runtime::Button>(GetScene(), "ChatOpenButton");
        if ((input.GetKeyDown(Platform::Key::Enter) && !input.GetState().imeHandledEnter &&
            !input.GetState().WasKeyHandledByIme(Platform::Key::Enter)) || (open && open->WasClickedThisFrame()))
        {
            mInputConsumed = true;
            BeginEditing();
        }
    }
}

void ChatController::UpdateScroll()
{
    auto* viewport = Find<Runtime::RectTransform>(GetScene(), "ChatViewport");
    auto* scroll = Find<Runtime::ScrollRect>(GetScene(), "ChatViewport");
    auto* content = Find<Runtime::RectTransform>(GetScene(), "ChatContent");
    const auto* context = GetRuntimeContext();
    if (!viewport || !scroll || !content || !context) return;
    const auto& input = context->GetInput();
    const auto cursor = input.GetMousePosition();
    const float wheel = input.GetMouseWheel();
    if (std::isfinite(wheel) && wheel != 0 && viewport->GetVisibleRect().Contains(
        static_cast<float>(cursor.GetX()), static_cast<float>(cursor.GetY())))
    {
        const auto* canvas = Find<Runtime::Canvas>(GetScene(), "ChatCanvas");
        const float scale = canvas ? canvas->GetScaleFactor() : 1;
        const float offset = Runtime::ScrollRect::ClampAxis(scroll->GetScrollOffset().GetY() - wheel * 48 * scale,
            viewport->GetResolvedRect().height, content->GetDesiredSize().height);
        scroll->SetScrollOffset({ 0, offset });
        const float maximum = (std::max)(0.0f, content->GetDesiredSize().height - viewport->GetResolvedRect().height);
        mFollowLatest = offset >= maximum - scale;
        mInputConsumed = true;
    }
    // 새 행의 높이는 다음 레이아웃에서 정해진다. 큰 오프셋을 요청하면 ScrollRect가 그때의 끝으로 제한한다.
    // 사용자가 위로 스크롤한 동안에는 이 추종을 멈춰 도착한 메시지가 읽던 위치를 빼앗지 않는다.
    if (mFollowLatest) scroll->SetScrollOffset({ 0, 1.0e9f });
}

void ChatController::AddMessage(const std::string& nickname, const std::string& text)
{
    std::string name = Utf8Prefix(nickname, 96);
    for (char& character : name) if (static_cast<unsigned char>(character) < 32) character = ' ';
    AppendRow(name + ": " + Utf8Prefix(text, MaximumMessageBytes), RowStyle::Player);
}

void ChatController::AddSystemMessage(const std::string& text)
{
    AppendRow(Utf8Prefix(text, MaximumMessageBytes), RowStyle::Feedback);
}

void ChatController::AddServerNotice(const NoticeKind kind, const std::string& text)
{
    AppendRow(Utf8Prefix(text, MaximumMessageBytes), kind == NoticeKind::Announcement
        ? RowStyle::Announcement : RowStyle::Membership);
}

void ChatController::AppendRow(std::string text, const RowStyle style)
{
    auto* scene = GetScene();
    auto* content = scene ? scene->FindGameObject("ChatContent") : nullptr;
    if (!content) return;
    while (mRows.size() >= MaximumMessages)
    {
        // Scene 삭제가 지연되는 프레임에도 오래된 행이 레이아웃과 화면에 남지 않도록 먼저 비활성화한다.
        if (auto* oldest = scene->GetGameObject(mRows.front())) oldest->SetActive(false);
        static_cast<void>(scene->RemoveGameObject(mRows.front()));
        mRows.pop_front();
    }
    auto* row = scene->CreateGameObject("ChatMessage");
    if (!row->GetTransform().SetParent(&content->GetTransform()))
    {
        static_cast<void>(scene->RemoveGameObject(row->GetInstanceId()));
        return;
    }
    auto* rect = row->AddComponent<Runtime::RectTransform>();
    rect->SetAnchorMin({ 0, 0 });
    rect->SetAnchorMax({ 1, 0 });
    rect->SetOffsetMin({ 0, 0 });
    rect->SetOffsetMax({ 0, 0 });
    auto* renderer = row->AddComponent<Runtime::TextRenderer>();
    renderer->SetText(std::move(text));
    renderer->SetFontFamily("Summit UI");
    renderer->SetFontSize(21);
    renderer->SetLineSpacing(1.15f);
    renderer->SetSortingOrder(510);
    renderer->SetSpace(Runtime::TextRenderer::Space::Screen);
    switch (style)
    {
    case RowStyle::Announcement: renderer->SetColor({ .45f, 1, .55f, 1 }); break;
    case RowStyle::Membership: renderer->SetColor({ 1, .85f, .3f, 1 }); break;
    case RowStyle::Feedback: renderer->SetColor({ .58f, .82f, .98f, 1 }); break;
    case RowStyle::Player: renderer->SetColor({ .94f, .96f, 1, 1 }); break;
    }
    auto* layout = row->AddComponent<Runtime::LayoutElement>();
    layout->SetFit(Runtime::LayoutElement::Fit::Vertical);
    layout->SetWrapping(true);
    layout->SetMinimumSize({ 0, 24 });
    mRows.push_back(row->GetInstanceId());
    if (mFollowLatest)
        if (auto* scroll = Find<Runtime::ScrollRect>(scene, "ChatViewport")) scroll->SetScrollOffset({ 0, 1.0e9f });
}
}
