#include "PlayerController.h"
#include "../UI/ChatController.h"

#include <algorithm>
#include <cmath>
#include <span>
#include <string>
#include <utility>

#include "Assets/Asset.h"
#include "Platform/IInput.h"
#include "Runtime/ComponentType.h"
#include "Runtime/AudioSource.h"
#include "Runtime/BoxCollider2D.h"
#include "Runtime/GameObject.h"
#include "Runtime/Input.h"
#include "Runtime/PropertyDescriptor.h"
#include "Runtime/Rigidbody2D.h"
#include "Runtime/RuntimeContext.h"
#include "Runtime/Scene.h"
#include "Runtime/SpriteAnimator.h"
#include "Runtime/SpriteRenderer.h"
#include "Runtime/TextRenderer.h"
#include "Runtime/Transform.h"

namespace Summit
{

namespace Runtime = GameEngine::Runtime;

namespace
{
    Runtime::PropertyDescriptor MakeAtlasProperty(
        std::string name,
        std::string displayName,
        const PlayerController::Character character,
        const PlayerController::AnimationState state)
    {
        return Runtime::MakeAssetProperty<PlayerController>(
            std::move(name), std::move(displayName), GameEngine::Assets::AssetType::Sprite,
            [character, state](const PlayerController& controller)
                -> const GameEngine::Assets::AssetReference&
            {
                return controller.GetAtlas(character, state);
            },
            [character, state](
                PlayerController& controller, GameEngine::Assets::AssetReference atlas)
            {
                controller.SetAtlas(character, state, std::move(atlas));
            },
            Runtime::PropertyTraits::OmitWhenInvalid);
    }

    std::span<const Runtime::PropertyDescriptor> PlayerControllerProperties()
    {
        static const Runtime::PropertyDescriptor properties[] = {
            Runtime::MakeProperty<PlayerController>(
                "moveSpeed", "Move Speed", &PlayerController::GetMoveSpeed,
                &PlayerController::SetMoveSpeed),
            Runtime::MakeProperty<PlayerController>(
                "jumpSpeed", "Jump Speed", &PlayerController::GetJumpSpeed,
                &PlayerController::SetJumpSpeed),
            Runtime::MakeProperty<PlayerController>(
                "respawnY", "Respawn Y Limit", &PlayerController::GetRespawnY,
                &PlayerController::SetRespawnY),
            Runtime::MakeEnumProperty<PlayerController>(
                "selectedCharacter", "Selected Character",
                { "characterA", "characterB", "characterC", "characterD", "characterE", "tdw" },
                &PlayerController::GetSelectedCharacter,
                &PlayerController::SetSelectedCharacter),
            MakeAtlasProperty(
                "characterAIdleAtlas", "Character A Idle Atlas",
                PlayerController::Character::CharacterA,
                PlayerController::AnimationState::Idle),
            MakeAtlasProperty(
                "characterAWalkAtlas", "Character A Walk Atlas",
                PlayerController::Character::CharacterA,
                PlayerController::AnimationState::Walk),
            MakeAtlasProperty(
                "characterAJumpAtlas", "Character A Jump Atlas",
                PlayerController::Character::CharacterA,
                PlayerController::AnimationState::Jump),
            MakeAtlasProperty(
                "characterBIdleAtlas", "Character B Idle Atlas",
                PlayerController::Character::CharacterB,
                PlayerController::AnimationState::Idle),
            MakeAtlasProperty(
                "characterBWalkAtlas", "Character B Walk Atlas",
                PlayerController::Character::CharacterB,
                PlayerController::AnimationState::Walk),
            MakeAtlasProperty(
                "characterBJumpAtlas", "Character B Jump Atlas",
                PlayerController::Character::CharacterB,
                PlayerController::AnimationState::Jump),
            MakeAtlasProperty(
                "characterCIdleAtlas", "Character C Idle Atlas",
                PlayerController::Character::CharacterC,
                PlayerController::AnimationState::Idle),
            MakeAtlasProperty(
                "characterCWalkAtlas", "Character C Walk Atlas",
                PlayerController::Character::CharacterC,
                PlayerController::AnimationState::Walk),
            MakeAtlasProperty(
                "characterCJumpAtlas", "Character C Jump Atlas",
                PlayerController::Character::CharacterC,
                PlayerController::AnimationState::Jump),
            MakeAtlasProperty(
                "characterDIdleAtlas", "Character D Idle Atlas",
                PlayerController::Character::CharacterD,
                PlayerController::AnimationState::Idle),
            MakeAtlasProperty(
                "characterDWalkAtlas", "Character D Walk Atlas",
                PlayerController::Character::CharacterD,
                PlayerController::AnimationState::Walk),
            MakeAtlasProperty(
                "characterDJumpAtlas", "Character D Jump Atlas",
                PlayerController::Character::CharacterD,
                PlayerController::AnimationState::Jump),
            MakeAtlasProperty(
                "characterEIdleAtlas", "Character E Idle Atlas",
                PlayerController::Character::CharacterE,
                PlayerController::AnimationState::Idle),
            MakeAtlasProperty(
                "characterEWalkAtlas", "Character E Walk Atlas",
                PlayerController::Character::CharacterE,
                PlayerController::AnimationState::Walk),
            MakeAtlasProperty(
                "characterEJumpAtlas", "Character E Jump Atlas",
                PlayerController::Character::CharacterE,
                PlayerController::AnimationState::Jump),
            MakeAtlasProperty(
                "tdwIdleAtlas", "tdw Idle Atlas",
                PlayerController::Character::Tdw,
                PlayerController::AnimationState::Idle),
            MakeAtlasProperty(
                "tdwWalkAtlas", "tdw Walk Atlas",
                PlayerController::Character::Tdw,
                PlayerController::AnimationState::Walk),
            MakeAtlasProperty(
                "tdwJumpAtlas", "tdw Jump Atlas",
                PlayerController::Character::Tdw,
                PlayerController::AnimationState::Jump),
            Runtime::MakeProperty<PlayerController>(
                "idleFrameRate", "Idle Frame Rate",
                &PlayerController::GetIdleFrameRate,
                &PlayerController::SetIdleFrameRate),
            Runtime::MakeProperty<PlayerController>(
                "walkFrameRate", "Walk Frame Rate",
                &PlayerController::GetWalkFrameRate,
                &PlayerController::SetWalkFrameRate),
            Runtime::MakeProperty<PlayerController>(
                "jumpFrameRate", "Jump Frame Rate",
                &PlayerController::GetJumpFrameRate,
                &PlayerController::SetJumpFrameRate),
        };
        return properties;
    }
}

const Runtime::ComponentType& PlayerController::StaticType()
{
    static const Runtime::ComponentType type{
        "PlayerController", &Runtime::MonoBehaviour::StaticType(), &PlayerControllerProperties,
        &Runtime::MakeComponentInstance<PlayerController> };
    return type;
}

void PlayerController::SetMoveSpeed(const float moveSpeed)
{
    mMoveSpeed = std::isfinite(moveSpeed) ? (std::max)(moveSpeed, 0.0f) : 0.0f;
}

void PlayerController::SetJumpSpeed(const float jumpSpeed)
{
    mJumpSpeed = std::isfinite(jumpSpeed) ? (std::max)(jumpSpeed, 0.0f) : 0.0f;
}

void PlayerController::SetSelectedCharacter(const Character character)
{
    const Character validated = static_cast<Character>(ToIndex(character));
    if (mSelectedCharacter != validated)
    {
        mSelectedCharacter = validated;
        mAnimationDirty = true;
    }
}

void PlayerController::SetRespawnY(const float respawnY)
{
    mRespawnY = std::isfinite(respawnY) ? respawnY : -10.0f;
}

void PlayerController::SetIdleFrameRate(const float frameRate)
{
    mIdleFrameRate = std::isfinite(frameRate) && frameRate > 0.0f ? frameRate : 0.0f;
    mAnimationDirty = true;
}

void PlayerController::SetWalkFrameRate(const float frameRate)
{
    mWalkFrameRate = std::isfinite(frameRate) && frameRate > 0.0f ? frameRate : 0.0f;
    mAnimationDirty = true;
}

void PlayerController::SetJumpFrameRate(const float frameRate)
{
    mJumpFrameRate = std::isfinite(frameRate) && frameRate > 0.0f ? frameRate : 0.0f;
    mAnimationDirty = true;
}

void PlayerController::CollectAssetReferences(
    std::vector<GameEngine::Assets::AssetReference>& references) const
{
    // 원격 캐릭터도 이 참조를 공유하므로 현재 선택뿐 아니라 모든 캐릭터의 전환용 시트를 미리 읽는다.
    for (const CharacterAtlases& character : mCharacterAtlases)
    {
        for (const GameEngine::Assets::AssetReference* const atlas :
            { &character.idle, &character.walk, &character.jump })
        {
            if (atlas->IsValid())
            {
                references.push_back(*atlas);
            }
        }
    }
}

void PlayerController::Start()
{
    if (const auto* object = GetGameObject()) mStartPosition = object->GetTransform().GetWorldPosition();
    ApplyAnimationState(AnimationState::Idle);
}

void PlayerController::Update(const float /*deltaTime*/)
{
    Runtime::GameObject* const gameObject = GetGameObject();
    const Runtime::RuntimeContext* const context = GetRuntimeContext();
    if (!gameObject || !context)
    {
        return;
    }

    Runtime::Rigidbody2D* const rigidbody = gameObject->GetComponent<Runtime::Rigidbody2D>();
    if (!rigidbody)
    {
        return;
    }

    // Update가 입력을 정하고 그 뒤 fixed physics가 움직인다. UI 잠금이나 Space 입력보다 먼저
    // 복귀하여 낙하 속도·이전 지면 접촉을 버리고, 같은 프레임에 점프/효과음을 다시 만들지 않는다.
    auto& transform = gameObject->GetTransform();
    if (transform.GetWorldPosition().GetY() < mRespawnY)
    {
        if (transform.SetWorldPosition(mStartPosition))
        {
            rigidbody->ResetMotion();
            ApplyAnimationState(AnimationState::Jump);
        }
        return;
    }

    const Runtime::Input& input = context->GetInput();
    const auto* manager = GetScene() ? GetScene()->FindGameObject("NetworkManager") : nullptr;
    const auto* chat = manager ? manager->GetComponent<ChatController>() : nullptr;
    // 플레이어가 네트워크/UI 매니저보다 먼저 갱신되어도 채팅을 여는 프레임부터 멈춘다.
    const bool chatConsumesInput = chat && (chat->IsEditing() || chat->WasInputConsumedThisFrame() ||
        input.GetKeyDown(GameEngine::Platform::Key::Enter) ||
        ((input.GetMouseButtonDown(GameEngine::Platform::MouseButton::Left) ||
            input.GetMouseButtonUp(GameEngine::Platform::MouseButton::Left)) && chat->IsPointerOverUi()));
    const bool acceptsInput = mGameplayInputEnabled &&
        !chatConsumesInput &&
        !input.GetKeyDown(GameEngine::Platform::Key::Escape);
    const bool movesLeft = acceptsInput && (input.GetKey(GameEngine::Platform::Key::A) ||
        input.GetKey(GameEngine::Platform::Key::Left));
    const bool movesRight = acceptsInput && (input.GetKey(GameEngine::Platform::Key::D) ||
        input.GetKey(GameEngine::Platform::Key::Right));
    const float horizontalInput = movesLeft == movesRight ? 0.0f : (movesRight ? 1.0f : -1.0f);

    GameEngine::Math::Vector2 velocity = rigidbody->GetVelocity();
    velocity = { horizontalInput * mMoveSpeed, velocity.GetY() };
    if (acceptsInput && input.GetKeyDown(GameEngine::Platform::Key::Space) && rigidbody->IsGrounded())
    {
        // 실제로 수락된 로컬 점프와 효과음을 함께 발생시킨다. 키 반복이나 공중 입력은 재생하지 않는다.
        velocity = { velocity.GetX(), mJumpSpeed };
        if (auto* audio = gameObject->GetComponent<Runtime::AudioSource>())
            static_cast<void>(audio->Play());
    }
    rigidbody->SetVelocity(velocity);

    if (horizontalInput < 0.0f)
    {
        mFacingLeft = true;
    }
    else if (horizontalInput > 0.0f)
    {
        mFacingLeft = false;
    }
    if (Runtime::SpriteRenderer* const renderer =
            FindVisualObject(*gameObject).GetComponent<Runtime::SpriteRenderer>())
    {
        renderer->SetFlipX(IsSpriteFlipped(mSelectedCharacter, mFacingLeft));
    }

    AnimationState requestedState = AnimationState::Idle;
    if (!rigidbody->IsGrounded() || std::abs(velocity.GetY()) > 0.01f)
    {
        requestedState = AnimationState::Jump;
    }
    else if (std::abs(velocity.GetX()) > 0.01f)
    {
        requestedState = AnimationState::Walk;
    }
    if (mAnimationDirty || requestedState != mAnimationState)
    {
        ApplyAnimationState(requestedState);
    }
}

std::size_t PlayerController::ToIndex(const Character character)
{
    const auto index = static_cast<std::size_t>(character);
    return index < CharacterCount ? index : 0;
}

const GameEngine::Assets::AssetReference& PlayerController::GetAtlas(
    const Character character, const AnimationState state) const
{
    const CharacterAtlases& atlases = mCharacterAtlases[ToIndex(character)];
    switch (state)
    {
    case AnimationState::Walk:
        return atlases.walk;
    case AnimationState::Jump:
        return atlases.jump;
    case AnimationState::Idle:
    default:
        return atlases.idle;
    }
}

void PlayerController::SetAtlas(
    const Character character,
    const AnimationState state,
    GameEngine::Assets::AssetReference atlas)
{
    CharacterAtlases& atlases = mCharacterAtlases[ToIndex(character)];
    switch (state)
    {
    case AnimationState::Walk:
        atlases.walk = std::move(atlas);
        break;
    case AnimationState::Jump:
        atlases.jump = std::move(atlas);
        break;
    case AnimationState::Idle:
    default:
        atlases.idle = std::move(atlas);
        break;
    }
    mAnimationDirty = true;
}

void PlayerController::ApplyAnimationState(const AnimationState state)
{
    Runtime::GameObject* const gameObject = GetGameObject();
    if (!gameObject)
    {
        return;
    }

    Runtime::GameObject& visual = FindVisualObject(*gameObject);
    Runtime::SpriteRenderer* const renderer = visual.GetComponent<Runtime::SpriteRenderer>();
    Runtime::SpriteAnimator* const animator = visual.GetComponent<Runtime::SpriteAnimator>();
    if (!renderer || !animator)
    {
        return;
    }

    AnimationState effectiveState = state;
    const GameEngine::Assets::AssetReference* atlas =
        &GetAtlas(mSelectedCharacter, effectiveState);
    if (!atlas->IsValid() && effectiveState != AnimationState::Idle)
    {
        effectiveState = AnimationState::Idle;
        atlas = &GetAtlas(mSelectedCharacter, effectiveState);
    }
    renderer->SetSprite(*atlas);
    renderer->SetFrame(0);
    renderer->SetFlipX(IsSpriteFlipped(mSelectedCharacter, mFacingLeft));
    if (&visual != gameObject)
    {
        const float scale = GetVisualScale(mSelectedCharacter);
        visual.GetTransform().SetScale({ scale, scale, 1.0f });
        visual.GetTransform().SetPosition({ 0, GetVisualOffsetY(mSelectedCharacter, effectiveState), 0 });
    }

    float frameRate = mIdleFrameRate;
    if (effectiveState == AnimationState::Walk)
    {
        frameRate = mWalkFrameRate;
    }
    else if (effectiveState == AnimationState::Jump)
    {
        frameRate = mJumpFrameRate;
    }

    animator->SetFirstFrame(0);
    animator->SetFrameCount(0);
    animator->SetFrameRate(frameRate);
    animator->SetLooping(true);
    // 이전 캐릭터의 왕복 설정이 남지 않도록 전환할 때마다 명시적으로 켜거나 끈다.
    animator->SetPingPong(mSelectedCharacter == Character::Tdw &&
        effectiveState == AnimationState::Idle);
    animator->SetPlaying(true);
    animator->Restart();

    // 대체 idle이 아닌 요청 상태를 기억해야 누락된 walk/jump 시트를 매 프레임 다시 시작하지 않는다.
    mAnimationState = state;
    mAnimationDirty = false;
}

Runtime::GameObject& PlayerController::FindVisualObject(Runtime::GameObject& player)
{
    for (Runtime::Transform* child : player.GetTransform().GetChildren())
    {
        if (Runtime::GameObject* object = child->GetGameObject(); object && object->GetName() == "Visual")
        {
            return *object;
        }
    }
    return player;
}

float PlayerController::GetVisualScale(const Character character)
{
    return character == Character::Tdw ? 2.0f : 1.0f;
}

bool PlayerController::IsSpriteFlipped(const Character character, const bool facingLeft)
{
    // tdw만 원본이 왼쪽을 향한다. 네트워크에는 원본 보정 전의 논리 방향을 보낸다.
    return facingLeft != (character == Character::Tdw);
}

float PlayerController::GetVisualOffsetY(const Character character, const AnimationState state) const
{
    // 원본 프레임 중심에서 발 기준까지의 픽셀 거리다. A~E는 시트의 아래 경계를
    // 기준으로 하여 원래 프레임 안에 그려진 달리기/점프의 상하 움직임을 유지한다.
    // tdw는 300px 캔버스 안에서 몸의 발 기준이 200px에 있고, 이펙트가 그 아래로
    // 뻗는다. 이펙트 끝을 발로 취급하면 캐릭터가 지면 위로 떠오른다.
    static constexpr std::array<std::array<float, 3>, CharacterCount> centerToFeetPixels{{
        { 128.0f, 128.0f, 120.0f }, // A: 256 / 256 / 240px
        { 126.0f, 126.0f, 133.0f }, // B: 252 / 252 / 266px
        { 120.0f, 120.0f, 120.0f }, // C: 240px
        { 120.0f, 135.0f, 120.0f }, // D: 240 / 270 / 240px
        { 120.0f, 120.0f, 120.0f }, // E: 240px
        { 50.0f, 50.0f, 50.0f },    // tdw: 200px - 300px / 2
    }};
    const std::size_t stateIndex = state == AnimationState::Walk ? 1 :
        state == AnimationState::Jump ? 2 : 0;
    const Runtime::GameObject* player = GetGameObject();
    const Runtime::BoxCollider2D* collider =
        player ? player->GetComponent<Runtime::BoxCollider2D>() : nullptr;
    const float feetY = collider
        ? collider->GetOffset().GetY() - collider->GetSize().GetY() * 0.5f : 0.0f;
    // 시트의 PPU는 100이다. 그림 배율을 적용한 발 거리만 월드 단위로 바꿔 공통 콜라이더 바닥에 맞춘다.
    return centerToFeetPixels[ToIndex(character)][stateIndex] * GetVisualScale(character) / 100.0f + feetY;
}

void PlayerController::SetGameplayInputEnabled(const bool enabled)
{
    mGameplayInputEnabled = enabled;
    if (!enabled && GetGameObject())
    {
        if (auto* body = GetGameObject()->GetComponent<Runtime::Rigidbody2D>())
        {
            // UI가 입력을 가져가도 낙하와 중력은 계속된다. 수평 조작 속도만 즉시 끊는다.
            body->SetVelocity({ 0, body->GetVelocity().GetY() });
        }
    }
}

void PlayerController::UpdateNameplate(Runtime::GameObject& player, const std::string& nickname) const
{
    Runtime::Scene* scene = player.GetScene();
    if (!scene)
    {
        return;
    }
    Runtime::GameObject* label = nullptr;
    for (Runtime::Transform* child : player.GetTransform().GetChildren())
    {
        if (child->GetGameObject()->GetName() == "NicknameLabel")
        {
            label = child->GetGameObject();
            break;
        }
    }
    if (!label)
    {
        // Visual의 배율·좌우 방향과 분리해 닉네임의 글자 크기와 읽는 방향을 유지한다.
        label = scene->CreateGameObject("NicknameLabel");
        if (!label || !label->GetTransform().SetParent(&player.GetTransform()))
        {
            if (label) scene->RemoveGameObject(label->GetInstanceId());
            return;
        }
        auto* text = label->AddComponent<Runtime::TextRenderer>();
        text->SetSpace(Runtime::TextRenderer::Space::World);
        text->SetFontFamily("Summit UI");
        text->SetFontSize(22);
        text->SetPixelsPerUnit(100);
        text->SetMaxWidth(0);
        text->SetAlignment(Runtime::TextRenderer::Alignment::Center);
        text->SetBackgroundColor({ 0.0f, 0.0f, 0.0f, 0.6f });
        text->SetBackgroundPadding({ 8.0f, 4.0f });
        text->SetSortingOrder(20);
        text->SetCastShadows(false);
        text->SetReceiveShadows(false);
    }
    const auto* collider = GetGameObject()
        ? GetGameObject()->GetComponent<Runtime::BoxCollider2D>() : nullptr;
    const float feetY = collider
        ? collider->GetOffset().GetY() - collider->GetSize().GetY() * 0.5f : 0.0f;
    label->GetTransform().SetPosition({ 0, feetY - 0.25f, 0 });
    if (auto* text = label->GetComponent<Runtime::TextRenderer>())
    {
        text->SetText(nickname);
    }
}

}
