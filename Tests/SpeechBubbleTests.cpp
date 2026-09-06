#include <cmath>
#include <array>
#include <cstddef>
#include <limits>
#include <string>

#include "ChatTestSupport.h"
#include "Gameplay/PlayerController.h"
#include "Assets/Asset.h"
#include "Runtime/BoxCollider2D.h"
#include "Runtime/SpriteRenderer.h"
#include "Runtime/TextRenderer.h"
#include "Runtime/Transform.h"
#include "UI/SpeechBubble.h"

namespace
{
namespace Runtime = GameEngine::Runtime;
using ChatTestSupport::Require;

Runtime::GameObject& Child(Runtime::GameObject& object, const char* name)
{
    for (auto* child : object.GetTransform().GetChildren())
        if (child->GetGameObject()->GetName() == name) return *child->GetGameObject();
    throw std::runtime_error("required speech bubble child is missing");
}

bool Near(const float a, const float b) { return std::abs(a - b) < .001f; }

void Settle(ChatTestSupport::Fixture& fixture)
{
    fixture.Step();
    fixture.Step();
    fixture.Step();
}

void CheckAuthoredHeadAnchors(ChatTestSupport::Fixture& fixture, Runtime::GameObject& player,
    Runtime::GameObject& bubble)
{
    using Controller = Summit::PlayerController;
    using Character = Controller::Character;
    using Animation = Controller::AnimationState;
    const auto& controller = *player.GetComponent<Controller>();
    auto& visual = Child(player, "Visual");
    auto& renderer = *visual.GetComponent<Runtime::SpriteRenderer>();
    for (const auto character : { Character::CharacterA, Character::CharacterB, Character::CharacterC,
        Character::CharacterD, Character::CharacterE, Character::Tdw })
        for (const auto animation : { Animation::Idle, Animation::Walk, Animation::Jump })
        {
            const auto& reference = controller.GetAtlas(character, animation);
            const auto* asset = fixture.game.GetAssetDatabase().FindAsset<GameEngine::Assets::Sprite>(reference);
            const auto image = fixture.game.GetAssetDatabase().LoadTexture(reference);
            Require(asset && image && image->IsValid(), "head anchors require each actual character atlas");
            const auto& sheet = asset->GetSheet();
            const auto width = image->width / static_cast<unsigned int>(sheet.columns);
            const auto height = image->height / static_cast<unsigned int>(sheet.rows);
            const float scale = Controller::GetVisualScale(character);
            visual.GetTransform().SetScale({ scale, scale, 1 });
            visual.GetTransform().SetPosition({ 0, controller.GetVisualOffsetY(character, animation), 0 });
            renderer.SetSprite(reference);
            for (int frame = 0; frame < sheet.GetFrameCount(); ++frame)
            {
                unsigned int top = height;
                for (unsigned int y = 0; y < height && top == height; ++y)
                    for (unsigned int x = 0; x < width; ++x)
                    {
                        const auto sx = static_cast<unsigned int>(frame % sheet.columns) * width + x;
                        const auto sy = static_cast<unsigned int>(frame / sheet.columns) * height + y;
                        if (std::to_integer<unsigned int>(image->pixels[(static_cast<std::size_t>(sy) * image->width + sx) * 4 + 3]) >= 32)
                        { top = y; break; }
                    }
                Require(top < height, "authored character frame unexpectedly contains no visible pixels");
                if (character == Character::Tdw && animation == Animation::Jump)
                {
                    Require(top == 54, "tdw jump effect fixture changed; review the separate body anchor");
                    top = 121; // Visually authored hair line, inside the surrounding jump effect.
                }
                const float expected = visual.GetTransform().GetPosition().GetY() +
                    (static_cast<float>(height) * .5f - static_cast<float>(top)) / asset->GetPixelsPerUnit() * scale;
                renderer.SetFrame(frame + sheet.GetFrameCount() * 2);
                for (const bool flipped : { false, true })
                {
                    renderer.SetFlipX(flipped);
                    bubble.Update(0);
                    Require(Near(bubble.GetTransform().GetPosition().GetY(), expected),
                        "speech failed to track the actual character/frame head independently of horizontal flip");
                }
            }
        }
    renderer.SetSprite(GameEngine::Assets::AssetReference::Parse("ffffffffffffffffffffffffffffffff"));
    bubble.Update(0);
    const auto& collider = *player.GetComponent<Runtime::BoxCollider2D>();
    Require(Near(bubble.GetTransform().GetPosition().GetY(),
        collider.GetOffset().GetY() + collider.GetSize().GetY() * .5f),
        "unknown sprite content must use the collider fallback without loading arbitrary assets");
}
}

void CheckSpeechBubble(const unsigned int scale)
{
    ChatTestSupport::Fixture fixture(scale);
    auto& player = *fixture.scene->FindGameObject("Player");
    auto* bubble = Summit::SpeechBubble::ShowMessage(player, "안녕");
    Require(bubble && Near(bubble->GetRemainingSeconds(), 6), "speech did not begin its six-second lifetime");
    auto& root = *bubble->GetGameObject();
    auto& body = Child(root, "SpeechBubbleText");
    auto& tail = Child(root, "SpeechBubbleTail");
    auto& text = *body.GetComponent<Runtime::TextRenderer>();
    const auto rootId = root.GetInstanceId();
    const auto bodyId = body.GetInstanceId();
    const auto tailId = tail.GetInstanceId();
    Require(!text.IsEnabled(), "speech used stale layout before the new message was measured");
    Settle(fixture);
    Require(text.IsEnabled() && tail.GetComponent<Runtime::TextRenderer>()->IsEnabled(),
        "measured speech did not become visible");
    Require(text.GetSpace() == Runtime::TextRenderer::Space::World &&
        text.GetBackgroundColor().a > .5f && text.GetBackgroundColor().a < 1,
        "speech must use readable translucent world text");
    GameEngine::Platform::TextRasterizationRequest natural;
    natural.text = text.GetText();
    natural.fontFamily = text.GetFontFamily();
    natural.fontSize = text.GetFontSize();
    natural.lineSpacing = text.GetLineSpacing();
    const auto measured = fixture.cache->Resolve(natural);
    Require(measured && Near(text.GetMaxWidth(), static_cast<float>(measured->width)),
        "a short bubble did not shrink to the exact unwrapped font measurement");
    const auto shortSize = body.GetComponent<Runtime::RectTransform>()->GetResolvedRect();
    Require(shortSize.width < 60 && shortSize.height < 40,
        "a two-character greeting still reserves a large chat rectangle");

    std::string longText;
    for (std::size_t index = 0; index < Summit::SpeechBubble::MaximumCodePoints - 1; ++index) longText += "한";
    const std::string expected = longText + "👋…";
    longText += "👋끝";
    root.Update(4);
    Require(Near(bubble->GetRemainingSeconds(), 2), "speech timer did not advance");
    Require(Summit::SpeechBubble::ShowMessage(player, longText) == bubble &&
        Near(bubble->GetRemainingSeconds(), 6) && root.GetInstanceId() == rootId,
        "replacement speech must reuse the character bubble and restart its timer");
    Require(text.GetText() == expected, "48-codepoint truncation split Korean/emoji or omitted the ellipsis");
    fixture.chat->AddMessage("Local", longText);
    bool completeHistory = false;
    for (const auto& [id, object] : fixture.scene->GetGameObjects())
    {
        static_cast<void>(id);
        if (const auto* row = object->GetComponent<Runtime::TextRenderer>(); row &&
            object->GetName() == "ChatMessage" && row->GetText() == "Local: " + longText)
            completeHistory = true;
    }
    Require(completeHistory, "shortening the bubble must not shorten the chat panel history");
    Settle(fixture);
    const auto desired = body.GetComponent<Runtime::RectTransform>()->GetDesiredSize();
    Require(desired.height > text.GetFontSize() * 2 && text.IsEnabled(),
        "long Korean speech did not wrap through the real font measurement path");
    const auto longSize = body.GetComponent<Runtime::RectTransform>()->GetResolvedRect();
    Require(longSize.width <= 240 && longSize.width > shortSize.width && longSize.height > shortSize.height &&
        longSize.height <= shortSize.height * 5.1f,
        "long speech did not stay within the compact width/five-line height budget");
    const float halfBackground = (desired.height * .5f + text.GetBackgroundPadding().GetY()) /
        text.GetPixelsPerUnit();
    Require(Near(body.GetTransform().GetPosition().GetY() - halfBackground, .16f),
        "the wrapped bubble must keep a small fixed gap above the actual head");

    Require(Summit::SpeechBubble::ShowMessage(player, "안녕") == bubble, "short replacement allocated a second bubble");
    Settle(fixture);
    const auto smallAgain = body.GetComponent<Runtime::RectTransform>()->GetResolvedRect();
    Require(Near(smallAgain.width, shortSize.width) && Near(smallAgain.height, shortSize.height),
        "a short replacement retained the previous long message's width or height");
    CheckAuthoredHeadAnchors(fixture, player, root);

    auto& visual = Child(player, "Visual");
    auto* sprite = visual.GetComponent<Runtime::SpriteRenderer>();
    sprite->SetFlipX(true);
    visual.GetTransform().SetScale({ 2, 2, 1 });
    visual.GetTransform().SetPosition({ 0, -.28f, 0 });
    sprite->SetSprite(player.GetComponent<Summit::PlayerController>()->GetAtlas(
        Summit::PlayerController::Character::Tdw, Summit::PlayerController::AnimationState::Idle));
    sprite->SetFrame(0);
    root.Update(0);
    Require(Near(root.GetTransform().GetPosition().GetY(), .30f) &&
        Near(body.GetTransform().GetScale().GetX(), 1),
        "enlarged/flipped characters must move speech above the head without flipping the text");
    const auto before = body.GetTransform().GetWorldPosition();
    const auto position = player.GetTransform().GetPosition();
    player.GetTransform().SetPosition({ position.GetX() + 3, position.GetY() + 2, position.GetZ() });
    const auto after = body.GetTransform().GetWorldPosition();
    Require(Near(after.GetX() - before.GetX(), 3) && Near(after.GetY() - before.GetY(), 2),
        "speech did not follow its character through the transform hierarchy");

    root.Update(std::numeric_limits<float>::quiet_NaN());
    root.Update(-1);
    Require(Near(bubble->GetRemainingSeconds(), 6), "invalid frame deltas corrupted the speech lifetime");
    root.Update(5.9f);
    Require(root.IsActive(), "speech disappeared before its deadline");
    root.Update(.11f);
    Require(!root.IsActive() && Near(bubble->GetRemainingSeconds(), 0), "expired speech remained visible");
    Require(Summit::SpeechBubble::ShowMessage(player, "다시 출발!") == bubble && root.IsActive(),
        "a new message could not reactivate an expired bubble");
    Require(fixture.scene->RemoveGameObject(player.GetInstanceId()) &&
        fixture.scene->GetGameObject(rootId) == nullptr && fixture.scene->GetGameObject(bodyId) == nullptr &&
        fixture.scene->GetGameObject(tailId) == nullptr,
        "removing a character left its speech or tail in the scene");
}

void RunSpeechBubbleTests()
{
    CheckSpeechBubble(1);
    CheckSpeechBubble(2);
}
