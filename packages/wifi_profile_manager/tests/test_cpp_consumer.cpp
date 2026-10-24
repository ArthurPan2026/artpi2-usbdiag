/* SPDX-License-Identifier: Apache-2.0 */
#include "wifi_profile_manager.h"

int main()
{
    wpm_core_t core{};
    wpm_core_adapter_state_t state{};
    wpm_core_init(&core);
    wpm_core_load(&core, nullptr, WPM_STORAGE_EMPTY);
    wpm_core_adapter_snapshot(&core, &state);
    return (state.phase == WPM_PHASE_IDLE &&
            state.storage == WPM_STORAGE_EMPTY) ? 0 : 1;
}
