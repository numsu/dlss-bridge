#include "dlss-bridge/abi/v1.h"
#include <stddef.h>

int main(void)
{
    DlssBridgeFrameContractV1 frame = {0};
    DlssBridgeCaptureAdapterV1 capture = {0};
    DlssBridgeExecutorV1 executor = {0};
    DlssBridgeTransportV1 transport = {0};
    frame.struct_size = sizeof(frame);
    frame.abi_version = DLSS_BRIDGE_ABI_V1;
    return !(frame.struct_size && sizeof(capture) && sizeof(executor) && sizeof(transport));
}
