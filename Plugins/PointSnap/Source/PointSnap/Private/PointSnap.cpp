#include "PointSnap.h"

DEFINE_LOG_CATEGORY(LogPointSnap);

#define LOCTEXT_NAMESPACE "FPointSnapModule"

void FPointSnapModule::StartupModule()
{
}

void FPointSnapModule::ShutdownModule()
{
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FPointSnapModule, PointSnap)
