#pragma once

#include <string_view>

namespace Summit
{

// 요청과 UI 선택은 0~4를 사용한다. 5는 닉네임 규칙으로 결정된 표시용 ID이며 직접 선택하지 않는다.
inline constexpr unsigned int SelectableCharacterCount = 5;
inline constexpr unsigned int EasterEggCharacter = 5;
inline constexpr unsigned int CharacterCount = 6;

/// <summary>별칭이나 공백 정규화 없이 확정할 닉네임 자체로 이스터에그를 판별한다.</summary>
[[nodiscard]] constexpr bool UsesEasterEggCharacter(const std::string_view nickname)
{
    return nickname == "상여자" || nickname == "심심이심셔";
}

/// <summary>일반 선택은 보존한 채 닉네임에 따라 실제로 표시할 캐릭터를 결정한다.</summary>
[[nodiscard]] constexpr unsigned int ResolveProfileCharacter(
    const std::string_view nickname, const unsigned int selectedCharacter)
{
    return UsesEasterEggCharacter(nickname) ? EasterEggCharacter
        : selectedCharacter < SelectableCharacterCount ? selectedCharacter : 0;
}

/// <summary>서버가 승인한 닉네임과 실제 캐릭터가 동일한 프로필 규칙을 만족하는지 확인한다.</summary>
[[nodiscard]] constexpr bool IsProfileCharacterValid(
    const std::string_view nickname, const unsigned int character)
{
    return character < CharacterCount &&
        character == ResolveProfileCharacter(nickname, character);
}

}
