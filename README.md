# 98Modem — Installation and Usage Guide

98Modem is a virtual COM port and Hayes-compatible modem for Windows 98. It
creates a COM3 device that any serial or modem application can open, and routes
calls over TCP/IP through a user-mode helper program.

## What You Need

The release zip contains three files:

| File | Purpose |
|---|---|
| `vmodem.vxd` | The virtual device driver |
| `vmodem.inf` | The hardware installation script |
| `vmodem_helper.exe` | The network helper application |

Place all three files in the same folder before you begin. A dedicated folder
such as `C:\98Modem` works well because the helper stores its configuration
(`vmodem.ini`) next to the executable.

---

## Installing the Driver

The driver installs through the standard Windows 98 hardware wizard.

1. Open **Control Panel** and double-click **Add New Hardware**.
2. Click **Next** on the welcome screen.
3. When asked whether Windows should search for new hardware, choose **No** and
   click **Next**.
4. In the hardware type list, select **Other devices** and click **Next**.
5. Click **Have Disk…** and browse to the folder containing `vmodem.inf`.
   Select the file and click **OK**.
6. The wizard shows **98Modem Virtual COM Port (COM3)**. Select it and click
   **Next**, then **Next** again to confirm.
7. If Windows asks for the location of `vmodem.vxd`, point it to the same
   folder.
8. Click **Finish**. Windows may ask you to restart — do so.

After restarting, open **Device Manager** (right-click My Computer → Properties
→ Device Manager). You should see **98Modem Virtual COM Port (COM3)** listed
under **Ports (COM & LPT)**. If it shows a yellow exclamation mark, verify that
`vmodem.vxd` is in the same folder as `vmodem.inf` and reinstall.

---

## Starting the Helper

The helper must be running for the modem to make or receive calls.

Double-click **vmodem_helper.exe**. The helper window opens and an icon appears
in the system tray. Closing the window hides it back to the tray — the helper
keeps running. To exit completely, right-click the tray icon and choose **Exit**.

You can add `vmodem_helper.exe` to your Startup folder so it launches
automatically when Windows starts.

---

## Helper Window Overview

The helper window has two main sections: **Listener** and **Phonebook**.

### Listener

The listener controls inbound connections — other systems calling your modem.

| Setting | Description |
|---|---|
| **Enable listener** | Check to allow inbound TCP connections |
| **Bind host** | Address to listen on (use `0.0.0.0` for all interfaces or `127.0.0.1` for local only) |
| **Port** | TCP port number for inbound connections (default 2323) |

Click **Apply** after changing listener settings. The status line below the
fields shows whether the listener is active.

If you only plan to dial out, you can leave the listener disabled.

### Phonebook

The phonebook maps phone-style dial strings to TCP/IP destinations. This lets
terminal programs that expect a phone number (such as HyperTerminal) dial by
name rather than typing a raw address.

Each entry has three fields:

| Field | Description |
|---|---|
| **Name** | A label for the entry (e.g. `Local BBS`) |
| **Number** | The dial string the application sends (e.g. `555-1212`) |
| **Target** | The TCP destination in `host:port` form (e.g. `bbs.example.com:23`) |

To add an entry, click **Add** and fill in all three fields.  
To change an entry, select it and click **Edit**.  
To remove an entry, select it and click **Delete**.

If you dial a phone-style number that is not in the phonebook, the helper
automatically opens an **Add Entry** dialog so you can map it before retrying
the call.

---

## Making a Call

Open COM3 in any terminal program. In HyperTerminal, choose **COM3** when
creating a new connection.

### Dialing by address

Type the address and port directly in the dial command:

```
ATDT bbs.example.com:23
```

The modem responds `CONNECT` when the TCP connection is established and `NO
CARRIER` if the connection fails or is refused.

### Dialing by phone number

If you have a phonebook entry with number `555-1212`:

```
ATDT 555-1212
```

Punctuation in the number is ignored during lookup — `555-1212` and `5551212`
match the same entry.

### Raw mode

Raw mode controls how the modem behaves immediately after a TCP connection is
established on an outbound dial.

**Raw mode ON — `AT+VRAW=1` (default)**

The modem emits `CONNECT` as soon as the TCP connection opens and begins passing
bytes immediately. This is the right choice when calling a standard Telnet BBS,
a shell server, or any host that is not running 98Modem on the other end. Almost
all outbound calls use this mode.

**Raw mode OFF — `AT+VRAW=0`**

The modem performs a brief vmodem-to-vmodem handshake after the TCP connection
opens. The caller sends a small `VMDM` identification sequence and waits for the
answering side to confirm before emitting `CONNECT`. This synchronizes both ends
so that `CONNECT` only appears after the remote user has actually typed `ATA`,
rather than the moment the TCP socket opens. Use this mode when calling another
machine that is also running 98Modem.

If the answering side does not respond to the handshake within 250 ms (because
it is not running 98Modem), it falls back to raw mode automatically and rings as
normal. The caller side may see the handshake bytes arrive as leading data in
that case.

`ATZ` resets raw mode back to ON.

**Per-phonebook-entry raw mode**

Phonebook entries each store their own raw mode setting that overrides `AT+VRAW`
for that entry. When you add or edit a phonebook entry, check or uncheck **Raw
mode** to set whether that destination expects the vmodem handshake. This lets
you keep some entries set for vmodem-to-vmodem calling and others set for
standard hosts without changing the global setting before each dial.

### AT command reference

| Command | Effect |
|---|---|
| `AT` | Test — responds `OK` |
| `ATDT <address>` | Dial (TCP connect) |
| `ATA` | Answer an incoming call |
| `ATH0` | Hang up |
| `ATO` | Return to data mode from command mode |
| `+++` | Enter command mode while connected (pause before and after) |
| `ATZ` | Reset modem to defaults |
| `ATS0=n` | Auto-answer after *n* rings (0 = disabled) |
| `AT+VRAW=1` | Raw mode ON — emit CONNECT immediately on TCP open (default) |
| `AT+VRAW=0` | Raw mode OFF — wait for vmodem-to-vmodem handshake before CONNECT |

---

## Receiving a Call

When another system connects to your listener port, the modem signals an
incoming call:

1. The `RI` line is asserted and the terminal program shows `RING`.
2. Additional `RING` messages appear at regular intervals while the call waits.
3. Type `ATA` to answer. The modem responds `CONNECT` and data mode begins.
4. An unanswered call times out after 30 seconds.

To auto-answer, set `ATS0=1` (answer after one ring) or higher. The setting
resets to 0 when you send `ATZ`.

---

## Hanging Up

From data mode, pause briefly, type `+++`, then pause again. This switches to
command mode. Then type:

```
ATH0
```

The modem responds `NO CARRIER` and the TCP connection closes.

Alternatively, dropping the DTR line through your terminal program's disconnect
function also hangs up.

---

## Using 98Modem from a DOS Window

98Modem also supports DOS programs running inside a Windows 98 DOS box. The
virtual modem appears as a standard serial COM port at the hardware register
level, so DOS programs that talk directly to the UART (rather than using BIOS
calls) work without any special setup.

Open a DOS window, set your DOS program to use the same COM port number shown
in Device Manager (COM3 by default), and operate the modem as normal. You
cannot use the Windows and DOS frontends simultaneously — whichever opens the
port first owns it until it closes.

---

## Configuration File

The helper saves settings to `vmodem.ini` in the same folder as
`vmodem_helper.exe`. You can edit this file in Notepad if the helper is not
running. The format is:

```ini
[listener]
enabled=0
bind_host=127.0.0.1
port=2323

[phonebook]
count=1

[phonebook.0]
name=Local BBS
number=555-1212
target=bbs.example.com:23
```

---

## Troubleshooting

**COM3 does not appear in Device Manager.**  
Reinstall the driver. Make sure `vmodem.vxd` is in the same folder as
`vmodem.inf` when running the Add New Hardware wizard.

**Terminal program cannot open COM3.**  
Verify the driver installed without errors in Device Manager. Make sure no
other application has the port open.

**ATDT returns NO CARRIER immediately.**  
Check that `vmodem_helper.exe` is running. Verify the target host and port are
reachable from your machine. Check the helper's status line for error
information.

**Inbound calls do not ring.**  
Check that the listener is enabled and the Apply button was clicked. Verify
that the listen port is reachable — check any firewall or router configuration
blocking the port.

**Dialing a phone number fails with an add-entry prompt.**  
The number is not in the phonebook. Fill in the target address in the dialog
and click OK; the dial retries automatically.
