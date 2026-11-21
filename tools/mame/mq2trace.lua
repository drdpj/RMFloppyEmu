-- RMFloppyEmu - a Raspberry Pi Pico 2 replacement for the RML 480Z's disk unit
-- Copyright (C) 2026 Daniel Jameson
--
-- This program is free software: you can redistribute it and/or modify
-- it under the terms of the GNU General Public License as published by
-- the Free Software Foundation, either version 3 of the License, or
-- (at your option) any later version.
--
-- This program is distributed in the hope that it will be useful,
-- but WITHOUT ANY WARRANTY; without even the implied warranty of
-- MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
-- GNU General Public License for more details.
--
-- You should have received a copy of the GNU General Public License
-- along with this program.  If not, see <https://www.gnu.org/licenses/>.

-- mq2trace.lua: MAME autoboot script for the rm480z driver.
-- Logs every byte on the 480Z <-> MQ2 serial link, plus the MQ2's FDC and drive-control
-- activity, as seen from the MQ2's Z80 I/O space. Optionally types keys on the 480Z.
--
-- Environment:
--   MQ2_KEYS  key sequence, "time:key name;time:key name", e.g. "2.0:b B;2.5:Carriage Return"
--             (key names are ioport field names; set MQ2_LISTKEYS=1 to print them).
--             "Shift+key name" holds Shift with the key, e.g. "3.0:Shift+- =" types '='.
--   MQ2_SNAP  comma-separated emulated times at which to save a screen snapshot
--
-- Output lines: "<emulated seconds> <EVENT> <hex>". EVENT is one of:
--   RX    byte the MQ2 read from the link (sent by the 480Z)
--   TX    byte the MQ2 wrote to the link (sent to the 480Z)
--   CTC0  write to the baud-rate CTC channel (time constant 0D = 9600, 01 = 125000)
--   FDC   WD1793 command register write;  TRK/SEC  track/sector register writes
--   SEL   drive select/side port (0E);  DEN  density port (0D)  (both logged only when they change)

local machine = manager.machine
local cpu = machine.devices[":rs232:rm_mq2:maincpu"]
local io = cpu.spaces["io"]

local function now() return machine.time:as_double() end
local function log(ev, v) print(string.format("%10.6f %s %02X", now(), ev, v)) end

local wnames = { [0xe6] = "TX", [0xe0] = "CTC0", [0x00] = "FDC", [0x01] = "TRK",
                 [0x02] = "SEC", [0x0e] = "SEL", [0x0d] = "DEN" }

-- Z80 I/O addresses are 16 bits (A8-A15 carry the accumulator), so tap the whole range
-- and decode the low byte, as the MQ2 hardware does.
-- The IDC firmware rewrites the drive-select and density ports constantly while idle, so those are
-- logged only when their value changes (otherwise a long run makes millions of identical lines).
local only_changes = { SEL = true, DEN = true }
local last = {}
mq2_taps = {
  io:install_write_tap(0x0000, 0xffff, "mq2trace_w", function(off, data, mask)
    local n = wnames[off & 0xff]
    if not n then return end
    if only_changes[n] then
      if last[n] == data then return end
      last[n] = data
    end
    log(n, data)
  end),
  io:install_read_tap(0x0000, 0xffff, "mq2trace_r", function(off, data, mask)
    if (off & 0xff) == 0xe6 then log("RX", data) end
  end),
}

-- Key presses via ioport fields (this driver does not support natural keyboard input)
local fields = {}
for _, port in pairs(machine.ioport.ports) do
  for name, f in pairs(port.fields) do fields[name] = f end
end
if os.getenv("MQ2_LISTKEYS") then
  local names = {}
  for n in pairs(fields) do names[#names + 1] = n end
  table.sort(names)
  print("keys: " .. table.concat(names, " | "))
end

-- Entries are split at a ';' that starts a new "time:" entry, since the "; +" key's own name holds a ';'.
local keys = {}
for t, k in string.gmatch((os.getenv("MQ2_KEYS") or "") .. ";0:", "([%d%.]+):(.-);%f[%d]") do
  keys[#keys + 1] = { tonumber(t), k }
end
local snaps = {}
for t in string.gmatch(os.getenv("MQ2_SNAP") or "", "[%d%.]+") do snaps[#snaps + 1] = tonumber(t) end

-- A shifted key: Shift goes down first and the key SHIFT_LEAD seconds later, so the 480Z's keyboard scan
-- sees Shift held when the key arrives; both are released together.
local SHIFT_LEAD = 0.05
local ki, si, held, release_at, pending, press_at = 1, 1, nil, 0, nil, 0
mq2_frame = emu.add_machine_frame_notifier(function()
  local t = now()
  if pending and t > press_at then
    pending:set_value(1); table.insert(held, pending); pending = nil
  end
  if held and not pending and t > release_at then
    for _, f in ipairs(held) do f:set_value(0) end
    held = nil
  end
  local k = keys[ki]
  if not held and k and t > k[1] then
    local name, shift = k[2], false
    if name:sub(1, 6) == "Shift+" then name, shift = name:sub(7), true end
    local f = fields[name]
    if f and (not shift or fields["Shift"]) then
      print(string.format("%10.6f KEY %s", t, k[2]))
      if shift then
        fields["Shift"]:set_value(1)
        held, pending, press_at = { fields["Shift"] }, f, t + SHIFT_LEAD
        release_at = press_at + 0.1
      else
        f:set_value(1); held = { f }; release_at = t + 0.1
      end
    else
      print("mq2trace: no such key '" .. k[2] .. "'")
    end
    ki = ki + 1
  end
  if snaps[si] and t > snaps[si] then machine.video:snapshot(); si = si + 1 end
end)

print("mq2trace: taps installed")
