// The runtime areas the game calls that are not implemented yet: every function its thunks use,
// by interface and slot. An area is offered to the title once one of its functions exists;
// implemented areas live in their own files (task_queue_api.cpp, offline.cpp).

#include "runtime/gdk/runtime.h"

namespace am::rt::gdk {

void add_system_interfaces(std::vector<Interface>& out) {
  constexpr auto Z = Returns::Zero;
  out.push_back({"XDisplay (HDR)", guid("03f0fe74-fdd9-4e5c-b630-f9339c47acc5"),
                 guid("35f07670-706e-4bfb-9476-090798c5ebf3"), {
      {4, "XDisplayTryEnableHdrMode", nullptr, Z},
  }});
  const GUID store = guid("0dd112ac-7c24-448c-b92b-3960fb5bd30c");
  out.push_back({"XStore (durables)", store, guid("2d42fea5-e71d-4b76-97cd-c50afbb3ae5d"), {
      {71, "XStoreAcquireLicenseForDurablesAsync", nullptr},
      {72, "XStoreAcquireLicenseForDurablesResult", nullptr},
  }});
  out.push_back({"XStore", store, guid("b09d803c-2414-4a05-82c6-66dfdc9e9a44"), {
      {3, "XStoreCreateContext", nullptr},
      {4, "XStoreCloseContextHandle", nullptr, Z},
      {5, "XStoreQueryAssociatedProductsAsync", nullptr},
      {6, "XStoreQueryAssociatedProductsResult", nullptr},
      {9, "XStoreQueryEntitledProductsAsync", nullptr},
      {10, "XStoreQueryEntitledProductsResult", nullptr},
      {15, "XStoreEnumerateProductsQuery", nullptr},
      {16, "XStoreProductsQueryHasMorePages", nullptr, Z},
      {17, "XStoreProductsQueryNextPageAsync", nullptr},
      {18, "XStoreProductsQueryNextPageResult", nullptr},
      {19, "XStoreCloseProductsQueryHandle", nullptr, Z},
      {20, "XStoreAcquireLicenseForPackageAsync", nullptr},
      {21, "XStoreAcquireLicenseForPackageResult", nullptr},
      {22, "XStoreIsLicenseValid", nullptr, Z},
      {23, "XStoreCloseLicenseHandle", nullptr, Z},
      {28, "XStoreQueryGameLicenseAsync", nullptr},
      {29, "XStoreQueryGameLicenseResult", nullptr},
      {33, "XStoreQueryConsumableBalanceRemainingAsync", nullptr},
      {34, "XStoreQueryConsumableBalanceRemainingResult", nullptr},
      {35, "XStoreReportConsumableFulfillmentAsync", nullptr},
      {37, "XStoreGetUserCollectionsIdAsync", nullptr},
      {38, "XStoreGetUserCollectionsIdResultSize", nullptr},
      {39, "XStoreGetUserCollectionsIdResult", nullptr},
      {40, "XStoreGetUserPurchaseIdAsync", nullptr},
      {41, "XStoreGetUserPurchaseIdResultSize", nullptr},
      {42, "XStoreGetUserPurchaseIdResult", nullptr},
      {49, "XStoreShowPurchaseUIAsync", nullptr},
      {53, "XStoreShowRedeemTokenUIAsync", nullptr},
      {55, "XStoreQueryGameAndDlcPackageUpdatesAsync", nullptr},
      {56, "XStoreQueryGameAndDlcPackageUpdatesResultCount", nullptr},
      {57, "XStoreQueryGameAndDlcPackageUpdatesResult", nullptr},
      {60, "XStoreDownloadAndInstallPackageUpdatesAsync", nullptr},
      {61, "XStoreDownloadAndInstallPackageUpdatesResult", nullptr},
      {62, "XStoreDownloadAndInstallPackagesAsync", nullptr},
      {63, "XStoreDownloadAndInstallPackagesResultCount", nullptr},
      {64, "XStoreDownloadAndInstallPackagesResult", nullptr},
      {65, "XStoreQueryPackageIdentifier", nullptr, Z},
      {66, "XStoreRegisterGameLicenseChanged", nullptr},
      {67, "XStoreUnregisterGameLicenseChanged", nullptr, Z},
      {68, "XStoreRegisterPackageLicenseLost", nullptr},
      {69, "XStoreUnregisterPackageLicenseLost", nullptr, Z},
      {75, "XStoreShowProductPageUIAsync", nullptr},
  }});
  const GUID launch = guid("1b339674-328d-4283-a200-3171f18d3639");
  out.push_back({"XLaunchUri / XDisplay deferral", launch, launch, {
      {3, "XLaunchUri", nullptr},
      {4, "XDisplayAcquireTimeoutDeferral", nullptr},
      {5, "XDisplayCloseTimeoutDeferralHandle", nullptr, Z},
  }});
  out.push_back({"XSpeechSynthesizer", guid("3e241977-6237-41e9-8559-102c2d9983f1"),
                 guid("d722b373-8c4d-4692-9e51-d6ad9b37aa7d"), {
      {8, "XSpeechSynthesizerEnumerateInstalledVoices", nullptr},
      {9, "XSpeechSynthesizerCreate", nullptr},
      {10, "XSpeechSynthesizerCloseHandle", nullptr},
      {11, "XSpeechSynthesizerSetDefaultVoice", nullptr},
      {12, "XSpeechSynthesizerSetCustomVoice", nullptr},
      {13, "XSpeechSynthesizerCreateStreamFromText", nullptr},
      {14, "XSpeechSynthesizerCloseStreamHandle", nullptr},
      {15, "XSpeechSynthesizerGetStreamDataSize", nullptr},
      {16, "XSpeechSynthesizerGetStreamData", nullptr},
      {21, "XSpeechSynthesizerCreateStreamFromSsml", nullptr},
  }});
  out.push_back({"XGameSave", guid("704c3f58-e629-4cc2-b197-30511b996fe2"),
                 guid("1bfff3af-f14a-40a3-8e35-9ada906593f9"), {
      {4, "XGameSaveInitializeProviderAsync", nullptr},
      {5, "XGameSaveInitializeProviderResult", nullptr},
      {6, "XGameSaveCloseProvider", nullptr, Z},
      {8, "XGameSaveGetRemainingQuotaAsync", nullptr},
      {9, "XGameSaveGetRemainingQuotaResult", nullptr},
      {11, "XGameSaveDeleteContainerAsync", nullptr},
      {12, "XGameSaveDeleteContainerResult", nullptr},
      {13, "XGameSaveGetContainerInfo", nullptr},
      {14, "XGameSaveEnumerateContainerInfo", nullptr},
      {16, "XGameSaveCreateContainer", nullptr},
      {17, "XGameSaveCloseContainer", nullptr, Z},
      {19, "XGameSaveEnumerateBlobInfoByName", nullptr},
      {20, "XGameSaveReadBlobData", nullptr},
      {21, "XGameSaveReadBlobDataAsync", nullptr},
      {22, "XGameSaveReadBlobDataResult", nullptr},
      {23, "XGameSaveCreateUpdate", nullptr},
      {24, "XGameSaveCloseUpdate", nullptr, Z},
      {25, "XGameSaveSubmitBlobWrite", nullptr},
      {28, "XGameSaveSubmitUpdateAsync", nullptr},
      {29, "XGameSaveSubmitUpdateResult", nullptr},
  }});
  out.push_back({"XAppCapture", guid("a4f1aee2-4bf1-4485-b008-a7c26d52ac27"),
                 guid("22e672d7-b4e3-406c-bd50-8f0d25236f9e"), {
      {5, "XAppCaptureTakeScreenshot", nullptr},
  }});
  out.push_back({"XPackage", guid("af406016-e850-4aa8-a88d-2f3dcb9dac7e"), guid("0b9d556e-ab70-43f4-baa0-9999ac4cb040"), {
      {3, "XPackageGetCurrentProcessPackageIdentifier", nullptr},
      {4, "XPackageIsPackagedProcess", nullptr, Z},
      {5, "XPackageCreateInstallationMonitor", nullptr},
      {6, "XPackageCloseInstallationMonitorHandle", nullptr, Z},
      {7, "XPackageGetInstallationProgress", nullptr, Z},
      {9, "XPackageRegisterInstallationProgressChanged", nullptr},
      {12, "XPackageFindChunkAvailability", nullptr},
      {14, "XPackageChangeChunkInstallOrder", nullptr},
      {16, "XPackageInstallChunksAsync", nullptr},
      {17, "XPackageInstallChunksResult", nullptr},
      {18, "XPackageEstimateDownloadSize", nullptr},
      {19, "XPackageUninstallChunks", nullptr},
      {25, "XPackageGetMountPath", nullptr},
      {26, "XPackageCloseMountHandle", nullptr, Z},
      {34, "XPackageUninstallPackage", nullptr, Z},
      {37, "XPackageMountWithUiAsync", nullptr},
      {38, "XPackageMountWithUiResult", nullptr},
      {39, "XPackageEnumeratePackages", nullptr},
  }});
  out.push_back({"XGameUi", guid("dfcd4649-4ff8-4043-ba07-35d607df98b0"), guid("eaf669df-5542-4590-99a3-8dc061f837cc"), {
      {7, "XGameUiShowPlayerProfileCardAsync", nullptr},
      {8, "XGameUiShowPlayerProfileCardResult", nullptr},
      {17, "XGameUiShowTextEntryAsync", nullptr},
      {18, "XGameUiShowTextEntryResultSize", nullptr},
      {19, "XGameUiShowTextEntryResult", nullptr},
  }});
  out.push_back({"XPersistentLocalStorage", guid("f4faf4d4-2d04-4fce-b3e0-474a713a3e84"),
                 guid("41a4e10c-5a7e-41d9-8c37-37bde62a07d6"), {
      {4, "XPersistentLocalStorageGetPath", nullptr},
      {5, "XPersistentLocalStorageGetSpaceInfo", nullptr},
  }});
}

}  // namespace am::rt::gdk
