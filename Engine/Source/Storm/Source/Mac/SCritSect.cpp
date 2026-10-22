// macOS counterpart of Storm/Source/Win64/SCritSect.cpp.

#include "Engine/Source/Base/Platform/Mac/MacSync.h"
#include "Engine/Source/Storm/H/SCritSect.h"

CCritSect::CCritSect() {
    MacCriticalSectionInit(&this->m_critsect);
}

CCritSect::~CCritSect() {
    MacCriticalSectionDelete(&this->m_critsect);
}

void CCritSect::Enter() {
    MacCriticalSectionEnter(&this->m_critsect);
}

void CCritSect::Enter(int32_t forwriting) {
    MacCriticalSectionEnter(&this->m_critsect);
}

void CCritSect::Leave() {
    MacCriticalSectionLeave(&this->m_critsect);
}

void CCritSect::Leave(int32_t fromwriting) {
    MacCriticalSectionLeave(&this->m_critsect);
}
