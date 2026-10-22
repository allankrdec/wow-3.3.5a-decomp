// macOS counterpart of Lock/Win64/Event.cpp: auto-reset events.

#include <cstdint>
#include "Engine/Source/Base/Platform/Mac/MacSync.h"
#include "Engine/Source/BlizzardCore/Source/System/Lock/Lock.h"

void Blizzard::Lock::EventCreate(Event& event) {
    event = MacEventCreate(false, false);
}

void Blizzard::Lock::EventDestroy(Event& event) {
    MacEventDestroy(event);
}

void Blizzard::Lock::EventSet(Event& event) {
    MacEventSet(event);
}

void Blizzard::Lock::EventWaitForever(Event& event) {
    MacEventWait(event, MAC_WAIT_INFINITE);
}

Blizzard::Lock::Event* Blizzard::Lock::EventWait(uint32_t timeout, Event& event) {
    return MacEventWait(event, timeout) ? &event : nullptr;
}

Blizzard::Lock::EventObject::EventObject() {
    this->m_event = MacEventCreate(false, false);
}

Blizzard::Lock::EventObject::~EventObject() {
    EventDestroy(this->m_event);
}
