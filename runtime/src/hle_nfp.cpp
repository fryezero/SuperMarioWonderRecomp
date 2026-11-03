// amiibo (nn::nfp), new in Wonder 1.2.1, answered as a console with no NFC reader. The game calls nfp::Initialize at
// boot (1.2.1 main+5ba880), and the sdk's Initialize aborts when it cannot open nfp:user, so the library is replaced
// at the API level: the availability event exists but never fires, and every device operation reports
// ResultNfcDeviceNotFound, which the game already has to handle for controllers without NFC.
#include <nx/runtime.h>

namespace nx {

namespace {

constexpr uint32_t kNfcDeviceNotFound = (64u << 9) | 115u;  // nn::nfp::ResultNfcDeviceNotFound, 2115-0064

void noop(CPU&) {}
void not_found(CPU& c) { c.x[0] = kNfcDeviceNotFound; }
void state_init(CPU& c) { c.x[0] = 0; }  // DeviceState_Init: no detection running

void AttachAvailabilityChangeEvent(CPU& c) {  // (nn::os::SystemEventType*)
    static uint64_t create = find_symbol("_ZN2nn2os17CreateSystemEventEPNS0_15SystemEventTypeENS0_14EventClearModeEb");
    if (!create) fatal("nfp: nn::os::CreateSystemEvent not found");
    c.x[1] = 0;  // EventClearMode_ManualClear
    c.x[2] = 0;  // not inter-process
    call(c, create);
    log("nfp: availability event attached (no NFC reader)");
}

}  // namespace

NX_HLE("_ZN2nn3nfp10InitializeEv", noop);
NX_HLE("_ZN2nn3nfp29AttachAvailabilityChangeEventEPNS_2os15SystemEventTypeE", AttachAvailabilityChangeEvent);
NX_HLE("_ZN2nn3nfp11ListDevicesEPNS0_12DeviceHandleEPii", not_found);
NX_HLE("_ZN2nn3nfp14GetDeviceStateERKNS0_12DeviceHandleE", state_init);
NX_HLE("_ZN2nn3nfp9GetNpadIdEPjRKNS0_12DeviceHandleE", not_found);
NX_HLE("_ZN2nn3nfp14StartDetectionERKNS0_12DeviceHandleE", not_found);
NX_HLE("_ZN2nn3nfp13StopDetectionERKNS0_12DeviceHandleE", not_found);
NX_HLE("_ZN2nn3nfp5MountERKNS0_12DeviceHandleENS0_9ModelTypeENS0_11MountTargetE", not_found);
NX_HLE("_ZN2nn3nfp5FlushERKNS0_12DeviceHandleE", not_found);
NX_HLE("_ZN2nn3nfp10GetTagInfoEPNS0_7TagInfoERKNS0_12DeviceHandleE", not_found);
NX_HLE("_ZN2nn3nfp12GetModelInfoEPNS0_9ModelInfoERKNS0_12DeviceHandleE", not_found);
NX_HLE("_ZN2nn3nfp19OpenApplicationAreaERKNS0_12DeviceHandleEj", not_found);
NX_HLE("_ZN2nn3nfp18GetApplicationAreaEPvPmRKNS0_12DeviceHandleEm", not_found);
NX_HLE("_ZN2nn3nfp18SetApplicationAreaERKNS0_12DeviceHandleEPKvm", not_found);
NX_HLE("_ZN2nn3nfp21CreateApplicationAreaERKNS0_12DeviceHandleERKNS0_25ApplicationAreaCreateInfoE", not_found);
NX_HLE("_ZN2nn3nfp23RecreateApplicationAreaERKNS0_12DeviceHandleERKNS0_25ApplicationAreaCreateInfoE", not_found);
NX_HLE("_ZN2nn3nfp13StartRestorerEPNS0_12DeviceHandleERKNS0_24AmiiboSettingsStartParamERKNS0_7TagInfoE", not_found);
NX_HLE("_ZN2nn3nfp19StartGameDataEraserEPNS0_12DeviceHandleERKNS0_24AmiiboSettingsStartParamERKNS0_7TagInfoE",
       not_found);
NX_HLE("_ZN2nn3nfp29StartNicknameAndOwnerSettingsEPNS0_12DeviceHandleEPbPNS0_12RegisterInfoERKNS0_"
       "24AmiiboSettingsStartParamERKNS0_7TagInfoE",
       not_found);
NX_HLE("_ZN2nn3nfp29StartNicknameAndOwnerSettingsEPNS0_12DeviceHandleEPbPNS0_12RegisterInfoERKNS0_"
       "24AmiiboSettingsStartParamERKNS0_7TagInfoERKS4_",
       not_found);

}  // namespace nx
