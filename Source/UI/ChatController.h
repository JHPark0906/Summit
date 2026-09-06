#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>

#include "Runtime/MonoBehaviour.h"

namespace Summit
{
/// <summary>채팅 입력과 최근 기록을 관리한다. 연결과 메시지 송신은 MultiplayerController가 소유한다.</summary>
class ChatController final : public GameEngine::Runtime::MonoBehaviour
{
public:
    static constexpr std::size_t MaximumMessages = 100;
    static constexpr std::size_t MaximumMessageBytes = 512;

    enum class NoticeKind { Announcement, Join, Leave };

    [[nodiscard]] static const GameEngine::Runtime::ComponentType& StaticType();
    [[nodiscard]] const GameEngine::Runtime::ComponentType& GetComponentType() const override { return StaticType(); }

    /// <summary>MultiplayerController가 게임 입력을 처리하기 전에 프레임마다 정확히 한 번 호출한다.</summary>
    void TickInput();
    [[nodiscard]] bool IsEditing() const { return mEditing; }
    [[nodiscard]] bool WasInputConsumedThisFrame() const { return mInputConsumed; }
    [[nodiscard]] bool IsPointerOverUi() const;
    [[nodiscard]] bool IsAvailable() const { return mAvailable; }
    void SetAvailable(bool available);
    void CancelEditing();
    [[nodiscard]] std::optional<std::string> ConsumeSubmittedMessage();
    void AddMessage(const std::string& nickname, const std::string& text);
    void AddSystemMessage(const std::string& text);
    void AddServerNotice(NoticeKind kind, const std::string& text);
    [[nodiscard]] std::size_t GetMessageCount() const { return mRows.size(); }

private:
    enum class RowStyle { Player, Feedback, Announcement, Membership };
    void BeginEditing();
    void Submit();
    void RefreshView();
    void AppendRow(std::string text, RowStyle style);
    void UpdateScroll();

    // 행 객체는 Scene이 소유한다. 최근 기록의 순서와 삭제 대상만 인스턴스 ID로 보관한다.
    std::deque<unsigned int> mRows;
    // UI와 네트워크 컨트롤러 사이의 단일 소비 슬롯이다. 입력창을 닫아도 제출 내용은 남는다.
    std::optional<std::string> mSubmitted;
    std::string mError;
    std::string mLastDraft;
    // IME 결과는 프레임 후반의 InputField가 반영한다. 같은 조합의 결과만 다음 틱에 한 번 보낸다.
    std::uint64_t mPendingImeComposition = 0;
    bool mSubmissionReady = false;
    bool mAvailable = false;
    bool mEditing = false;
    bool mInputConsumed = false;
    bool mFollowLatest = true;
};
}
