#include "SpeechBubble.h"
#include "Serialization/RuntimeComponentFactories.h"

namespace Summit
{
namespace
{
    const bool gSpeechBubbleRegistered =
        GameEngine::Serialization::RegisterComponentType(SpeechBubble::StaticType());
}
}
