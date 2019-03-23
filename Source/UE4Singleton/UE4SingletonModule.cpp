#include "Modules/ModuleInterface.h"

class FUE4SingletonPlugin final : public IModuleInterface
{
public:
	// IModuleInterface implementation
	virtual void StartupModule() override {}
	virtual void ShutdownModule() override {}
	// End of IModuleInterface implementation
};

IMPLEMENT_MODULE(FUE4SingletonPlugin, UE4Singleton)
