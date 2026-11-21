/*
 * RMFloppyEmu - a Raspberry Pi Pico 2 replacement for the RML 480Z's disk unit
 * Copyright (C) 2026 Daniel Jameson
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

// Compiles every src/flux header as C++, as Disk Wrangler (C++17) will include them. Linking against the
// C objects proves the extern "C" guards are right.
#include "flux/crc16.h"
#include "flux/flux_pll.h"
#include "flux/hfe.h"
#include "flux/ibm_track.h"
#include "flux/scp.h"

int main() {
    const uint8_t b[1] = {0};
    ibm_track_t t;
    ibm_track_init(&t, nullptr, 0, nullptr, 0);
    return (flux_crc16(FLUX_CRC16_INIT, b, 1) != 0 && hfe_status_name(HFE_OK) != nullptr &&
            scp_status_name(SCP_OK) != nullptr) ? 0 : 1;
}
