using System;
using System.Runtime.InteropServices;

namespace LeonidaLauncher;

public static class NativeMethods
{
    // Multimedia Timer (winmm.dll)
    [DllImport("winmm.dll", EntryPoint = "timeBeginPeriod", SetLastError = true)]
    public static extern uint TimeBeginPeriod(uint uMilliseconds);

    [DllImport("winmm.dll", EntryPoint = "timeEndPeriod", SetLastError = true)]
    public static extern uint TimeEndPeriod(uint uMilliseconds);

    // Process Priority (kernel32.dll)
    public const uint REALTIME_PRIORITY_CLASS = 0x00000100;
    public const uint HIGH_PRIORITY_CLASS = 0x00000080;
    public const uint ABOVE_NORMAL_PRIORITY_CLASS = 0x00008000;
    public const uint NORMAL_PRIORITY_CLASS = 0x00000020;

    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern IntPtr OpenProcess(uint processAccess, bool bInheritHandle, int processId);

    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool SetPriorityClass(IntPtr hProcess, uint dwPriorityClass);

    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool CloseHandle(IntPtr hObject);

    // XInput Interop
    [StructLayout(LayoutKind.Sequential)]
    public struct XINPUT_GAMEPAD
    {
        public ushort wButtons;
        public byte bLeftTrigger;
        public byte bRightTrigger;
        public short sThumbLX;
        public short sThumbLY;
        public short sThumbRX;
        public short sThumbRY;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct XINPUT_STATE
    {
        public uint dwPacketNumber;
        public XINPUT_GAMEPAD Gamepad;
    }

    public const ushort XINPUT_GAMEPAD_DPAD_UP = 0x0001;
    public const ushort XINPUT_GAMEPAD_DPAD_DOWN = 0x0002;
    public const ushort XINPUT_GAMEPAD_DPAD_LEFT = 0x0004;
    public const ushort XINPUT_GAMEPAD_DPAD_RIGHT = 0x0008;
    public const ushort XINPUT_GAMEPAD_START = 0x0010;
    public const ushort XINPUT_GAMEPAD_BACK = 0x0020;
    public const ushort XINPUT_GAMEPAD_LEFT_THUMB = 0x0040;
    public const ushort XINPUT_GAMEPAD_RIGHT_THUMB = 0x0080;
    public const ushort XINPUT_GAMEPAD_LEFT_SHOULDER = 0x0100;
    public const ushort XINPUT_GAMEPAD_RIGHT_SHOULDER = 0x0200;
    public const ushort XINPUT_GAMEPAD_A = 0x1000;
    public const ushort XINPUT_GAMEPAD_B = 0x2000;
    public const ushort XINPUT_GAMEPAD_X = 0x4000;
    public const ushort XINPUT_GAMEPAD_Y = 0x8000;

    public const uint ERROR_SUCCESS = 0;
    public const uint ERROR_DEVICE_NOT_CONNECTED = 1167;

    [DllImport("xinput1_4.dll", EntryPoint = "XInputGetState", SetLastError = true)]
    private static extern uint XInputGetState_14(uint dwUserIndex, ref XINPUT_STATE pState);

    [DllImport("xinput1_3.dll", EntryPoint = "XInputGetState", SetLastError = true)]
    private static extern uint XInputGetState_13(uint dwUserIndex, ref XINPUT_STATE pState);

    [DllImport("xinput9_1_0.dll", EntryPoint = "XInputGetState", SetLastError = true)]
    private static extern uint XInputGetState_910(uint dwUserIndex, ref XINPUT_STATE pState);

    private static int _xinputVersion = -1;

    public static uint GetGamepadState(uint userIndex, ref XINPUT_STATE state)
    {
        if (_xinputVersion == -1)
        {
            try
            {
                var res = XInputGetState_14(userIndex, ref state);
                _xinputVersion = 14;
                return res;
            }
            catch (DllNotFoundException)
            {
                try
                {
                    var res = XInputGetState_13(userIndex, ref state);
                    _xinputVersion = 13;
                    return res;
                }
                catch (DllNotFoundException)
                {
                    try
                    {
                        var res = XInputGetState_910(userIndex, ref state);
                        _xinputVersion = 910;
                        return res;
                    }
                    catch (DllNotFoundException)
                    {
                        _xinputVersion = 0; // Not available
                        return ERROR_DEVICE_NOT_CONNECTED;
                    }
                }
            }
        }

        switch (_xinputVersion)
        {
            case 14:
                return XInputGetState_14(userIndex, ref state);
            case 13:
                return XInputGetState_13(userIndex, ref state);
            case 910:
                return XInputGetState_910(userIndex, ref state);
            default:
                return ERROR_DEVICE_NOT_CONNECTED;
        }
    }
}
