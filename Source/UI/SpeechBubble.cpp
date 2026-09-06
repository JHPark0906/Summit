#include "SpeechBubble.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>

#include "Assets/AssetReference.h"
#include "Runtime/BoxCollider2D.h"
#include "Runtime/Canvas.h"
#include "Runtime/GameObject.h"
#include "Runtime/LayoutElement.h"
#include "Runtime/PropertyDescriptor.h"
#include "Runtime/RectTransform.h"
#include "Runtime/Scene.h"
#include "Runtime/SpriteRenderer.h"
#include "Runtime/TextRenderer.h"
#include "Runtime/Transform.h"

namespace Summit
{
namespace
{
namespace Runtime = GameEngine::Runtime;
constexpr float PixelsPerUnit = 55.0f;
// Text width and padding are logical pixels; HeadGap is in world units above the body anchor.
constexpr float MaximumWidth = 240.0f;
constexpr float PaddingY = 4.0f;
constexpr float HeadGap = .16f;

struct SpriteHeadAnchor
{
    GameEngine::Assets::AssetReference atlas;
    int frameHeight;
    int frameCount;
    std::array<int, 5> topPixels;
};

float HeadOffset(const Runtime::SpriteRenderer& sprite)
{
    using GameEngine::Assets::AssetReference;
    // Content authoring data, like PlayerController's feet anchors: top opaque pixel
    // (alpha >= 32) in each source frame, measured without changing the PNGs. All use PPU100.
    // tdw's jump uses the character's hair at y121; its surrounding effect reaches y54.
    // Atlas/frame edits must update this table; SpeechBubbleTests compares the actual pixels.
    static const SpriteHeadAnchor anchors[]{
        { AssetReference::Parse("0cd8e6346c55411698759373e337f6af"), 256, 4, {64,72,56,56} },
        { AssetReference::Parse("a9c0c649947c4a2d8cc0f049299a7d58"), 256, 4, {56,64,56,64} },
        { AssetReference::Parse("ff345164dbfb4c279dfd57ae1057000b"), 240, 5, {40,40,48,56,40} },
        { AssetReference::Parse("ad7193d165944b21aedfb98787a9e992"), 252, 4, {21,28,28,28} },
        { AssetReference::Parse("02c173eb395944fab5af9f71d267e3d4"), 252, 4, {21,35,35,28} },
        { AssetReference::Parse("4785bbaf93454b81a5494fa3c6b320fd"), 266, 4, {0,7,14,7} },
        { AssetReference::Parse("301b138d467342d68a65a1b6584153ca"), 240, 4, {54,48,54,48} },
        { AssetReference::Parse("58082fe3782a4e99bfbfa348648d1a02"), 240, 4, {42,30,42,30} },
        { AssetReference::Parse("215a1c935a9147b08b406b619d2b0986"), 240, 3, {18,18,18} },
        { AssetReference::Parse("2ad03558577e451fabcd4274ad23fc2d"), 240, 4, {30,36,42,36} },
        { AssetReference::Parse("9cfe8ab6e19342e9bc7879aa2bb1a329"), 270, 4, {72,72,78,78} },
        { AssetReference::Parse("f6ec419c2c4440f583a2366d54abca25"), 240, 4, {18,18,18,18} },
        { AssetReference::Parse("4e1bd9d5749342e1ba764f5bdf5aa6de"), 240, 4, {30,36,42,36} },
        { AssetReference::Parse("3d004d1d7b36494b94099ab0f5b64e10"), 240, 4, {6,0,6,12} },
        { AssetReference::Parse("c5e22a420974457d8eb8a151e0cddae6"), 240, 4, {6,6,6,6} },
        { AssetReference::Parse("a6883f3e0ef64af8be111b537ca82395"), 300, 3, {121,121,121} },
        { AssetReference::Parse("5fa4a5aaec4a4b28a559717582e9dfee"), 300, 4, {121,122,121,122} },
        { AssetReference::Parse("1d0051e09a6d4b7ea3c306cdc7c29bd3"), 300, 1, {121} },
    };
    for (const auto& anchor : anchors)
        if (anchor.atlas == sprite.GetSprite())
        {
            const int frame = sprite.ResolveFrame(anchor.frameCount);
            const auto index = static_cast<std::size_t>((frame % anchor.frameCount + anchor.frameCount) % anchor.frameCount);
            return (static_cast<float>(anchor.frameHeight) * .5f - anchor.topPixels[index]) / 100.0f;
        }
    return -1; // Unknown content falls back to the character collider, without runtime asset I/O.
}

std::string DisplayText(const std::string_view message)
{
    std::size_t end = 0;
    std::size_t count = 0;
    while (end < message.size() && count < SpeechBubble::MaximumCodePoints)
    {
        ++end;
        // Skip only UTF-8 continuation bytes, so the limit never cuts a Korean/emoji scalar.
        while (end < message.size() && (static_cast<unsigned char>(message[end]) & 0xc0) == 0x80) ++end;
        ++count;
    }
    std::string result(message.substr(0, end));
    if (end < message.size()) result += "…";
    return result;
}

Runtime::TextRenderer* AddWorldText(Runtime::GameObject& object)
{
    auto* text = object.AddComponent<Runtime::TextRenderer>();
    text->SetSpace(Runtime::TextRenderer::Space::World);
    text->SetFontFamily("Summit UI");
    text->SetFontSize(22);
    text->SetPixelsPerUnit(PixelsPerUnit);
    text->SetCastShadows(false);
    text->SetReceiveShadows(false);
    text->SetEnabled(false);
    return text;
}
}

const Runtime::ComponentType& SpeechBubble::StaticType()
{
    static const Runtime::ComponentType type{ "SpeechBubble", &Runtime::MonoBehaviour::StaticType(),
        nullptr, &Runtime::MakeComponentInstance<SpeechBubble> };
    return type;
}

SpeechBubble* SpeechBubble::ShowMessage(Runtime::GameObject& player, const std::string_view message,
    const float remainingSeconds)
{
    auto* scene = player.GetScene();
    if (!scene || message.empty() || !std::isfinite(remainingSeconds) || remainingSeconds <= 0) return nullptr;
    SpeechBubble* bubble = nullptr;
    for (auto* child : player.GetTransform().GetChildren())
        if (auto* candidate = child->GetGameObject()->GetComponent<SpeechBubble>())
        {
            bubble = candidate;
            break;
        }
    if (!bubble)
    {
        // Parent to the player root, not Visual, so sprite flips and character scaling never mirror the text.
        auto* root = scene->CreateGameObject("SpeechBubble");
        if (!root || !root->GetTransform().SetParent(&player.GetTransform()))
        {
            if (root) static_cast<void>(scene->RemoveGameObject(root->GetInstanceId()));
            return nullptr;
        }
        bubble = root->AddComponent<SpeechBubble>();
        // Canvas supplies the existing text measurement/layout pass. Text and its background
        // still render in world space; no screen overlay or separate font metrics are involved.
        static_cast<void>(root->AddComponent<Runtime::Canvas>());
        auto* body = scene->CreateGameObject("SpeechBubbleText");
        auto* tail = scene->CreateGameObject("SpeechBubbleTail");
        if (!body || !tail || !body->GetTransform().SetParent(&root->GetTransform()) ||
            !tail->GetTransform().SetParent(&root->GetTransform()))
        {
            if (body) static_cast<void>(scene->RemoveGameObject(body->GetInstanceId()));
            if (tail) static_cast<void>(scene->RemoveGameObject(tail->GetInstanceId()));
            static_cast<void>(scene->RemoveGameObject(root->GetInstanceId()));
            return nullptr;
        }
        bubble->mTextId = body->GetInstanceId();
        bubble->mTailId = tail->GetInstanceId();
        auto* rect = body->AddComponent<Runtime::RectTransform>();
        rect->SetOffsetMax({ MaximumWidth, 0 });
        auto* layout = body->AddComponent<Runtime::LayoutElement>();
        layout->SetFit(Runtime::LayoutElement::Fit::Vertical);
        layout->SetWrapping(true);
        auto* text = AddWorldText(*body);
        text->SetMaxWidth(MaximumWidth);
        text->SetLineSpacing(1.12f);
        text->SetColor({ .96f, .97f, 1.0f, 1.0f });
        text->SetBackgroundColor({ .055f, .075f, .11f, .88f });
        text->SetBackgroundPadding({ 6, PaddingY });
        text->SetSortingOrder(40);
        auto* tailText = AddWorldText(*tail);
        tailText->SetText("▼");
        tailText->SetFontSize(10);
        tailText->SetColor(text->GetBackgroundColor());
        tailText->SetSortingOrder(39);
        tail->GetTransform().SetPosition({ 0, .09f, 0 });
    }
    auto* body = scene->GetGameObject(bubble->mTextId);
    auto* tail = scene->GetGameObject(bubble->mTailId);
    if (!body || !tail) return nullptr;
    body->GetComponent<Runtime::TextRenderer>()->SetText(DisplayText(message));
    body->GetComponent<Runtime::TextRenderer>()->SetEnabled(false);
    body->GetComponent<Runtime::TextRenderer>()->SetMaxWidth(0);
    body->GetComponent<Runtime::RectTransform>()->SetOffsetMax({ 0, 0 });
    body->GetComponent<Runtime::LayoutElement>()->SetWrapping(false);
    body->GetComponent<Runtime::LayoutElement>()->SetFit(Runtime::LayoutElement::Fit::Both);
    tail->GetComponent<Runtime::TextRenderer>()->SetEnabled(false);
    bubble->mRemainingSeconds = (std::min)(remainingSeconds, DisplaySeconds);
    // A reused bubble may still update in the same frame as ShowMessage. Wait through that
    // frame's layout before using the measured height of the new message.
    bubble->mLayoutUpdatesToWait = 2;
    bubble->mMeasureUnwrapped = true;
    bubble->GetGameObject()->SetActive(true);
    bubble->UpdatePosition();
    return bubble;
}

void SpeechBubble::UpdatePosition()
{
    auto& transform = GetGameObject()->GetTransform();
    auto* parent = transform.GetParent();
    float headY = 1.28f;
    if (parent)
        if (const auto* collider = parent->GetGameObject()->GetComponent<Runtime::BoxCollider2D>())
            headY = collider->GetOffset().GetY() + collider->GetSize().GetY() * .5f;
    if (parent)
        for (auto* child : parent->GetChildren())
            if (child->GetGameObject()->GetName() == "Visual")
            {
                if (const auto* sprite = child->GetGameObject()->GetComponent<Runtime::SpriteRenderer>())
                    if (const float offset = HeadOffset(*sprite); offset >= 0)
                        headY = child->GetPosition().GetY() + offset * std::abs(child->GetScale().GetY());
                break;
            }
    transform.SetPosition({ 0, headY, 0 });
}

void SpeechBubble::Update(const float deltaTime)
{
    if (std::isfinite(deltaTime) && deltaTime > 0)
        mRemainingSeconds = (std::max)(0.0f, mRemainingSeconds - deltaTime);
    if (mRemainingSeconds <= 0)
    {
        // Keep one reusable child per player; deleting the player removes the whole bubble hierarchy.
        GetGameObject()->SetActive(false);
        return;
    }
    UpdatePosition();
    if (mLayoutUpdatesToWait != 0 && --mLayoutUpdatesToWait != 0) return;
    auto* scene = GetScene();
    auto* body = scene ? scene->GetGameObject(mTextId) : nullptr;
    auto* tail = scene ? scene->GetGameObject(mTailId) : nullptr;
    if (!body || !tail) return;
    auto* rect = body->GetComponent<Runtime::RectTransform>();
    auto* text = body->GetComponent<Runtime::TextRenderer>();
    if (mMeasureUnwrapped)
    {
        const float width = rect->GetDesiredSize().width;
        if (!std::isfinite(width) || width <= 0) return;
        const float fittedWidth = (std::min)(std::ceil(width), MaximumWidth);
        // The renderer's maxWidth is a block width, not just a wrapping limit. Use the
        // actual unwrapped measurement so short messages also shrink horizontally.
        rect->SetOffsetMax({ fittedWidth, 0 });
        text->SetMaxWidth(fittedWidth);
        body->GetComponent<Runtime::LayoutElement>()->SetWrapping(true);
        body->GetComponent<Runtime::LayoutElement>()->SetFit(Runtime::LayoutElement::Fit::Vertical);
        mMeasureUnwrapped = false;
        return; // This frame's layout now measures the height at the selected width.
    }
    const float height = rect->GetDesiredSize().height;
    if (!std::isfinite(height) || height <= 0) return;
    body->GetTransform().SetPosition({ 0, HeadGap + (height * .5f + PaddingY) / PixelsPerUnit, 0 });
    text->SetEnabled(true);
    tail->GetComponent<Runtime::TextRenderer>()->SetEnabled(true);
}
}
