#include "ChatController.h"
#include "Serialization/RuntimeComponentFactories.h"

namespace Summit
{
namespace
{
    const bool gChatControllerRegistered =
        GameEngine::Serialization::RegisterComponentType(ChatController::StaticType());
}
}
