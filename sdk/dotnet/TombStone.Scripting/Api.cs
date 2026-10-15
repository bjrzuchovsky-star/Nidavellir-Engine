// Mirror of TsScriptApi (sdk/include/tombstone/script_abi.h, TS_SCRIPT_ABI 2).
using System;
using System.Runtime.InteropServices;

namespace TombStone.Native
{
    [StructLayout(LayoutKind.Sequential)]
    public unsafe struct TsScriptApi
    {
        public uint Abi;
        public IntPtr World;
        public delegate* unmanaged[Cdecl]<IntPtr, ulong> Tick;
        public delegate* unmanaged[Cdecl]<IntPtr, byte*, ulong> Find;
        public delegate* unmanaged[Cdecl]<IntPtr, int, ulong> Player;
        public delegate* unmanaged[Cdecl]<IntPtr, ulong, float*, float*, int> Position;
        public delegate* unmanaged[Cdecl]<IntPtr, ulong, float, float, void> Move;
        public delegate* unmanaged[Cdecl]<IntPtr, ulong, int, void> SetVisible;
        public delegate* unmanaged[Cdecl]<IntPtr, ulong, int, void> SetCollider;
        public delegate* unmanaged[Cdecl]<IntPtr, ulong, byte*, int, int> Play;
        public delegate* unmanaged[Cdecl]<IntPtr, byte*, float, float, ulong> Spawn;
        public delegate* unmanaged[Cdecl]<IntPtr, ulong, void> Destroy;
        public delegate* unmanaged[Cdecl]<IntPtr, ulong, byte*, double*, int> GetNumber;
        public delegate* unmanaged[Cdecl]<IntPtr, ulong, byte*, double, void> SetNumber;
        public delegate* unmanaged[Cdecl]<IntPtr, ulong, byte*, byte*, int, int> GetText;
        public delegate* unmanaged[Cdecl]<IntPtr, ulong, byte*, byte*, void> SetText;
        public delegate* unmanaged[Cdecl]<IntPtr, int, byte*, double, ulong, void> Toast;
        public delegate* unmanaged[Cdecl]<IntPtr, int, byte*, ulong, void> Log;
        public delegate* unmanaged[Cdecl]<IntPtr, byte*, float, ulong, void> Sound;
        public delegate* unmanaged[Cdecl]<IntPtr, byte*, float, void> Music;
        public delegate* unmanaged[Cdecl]<IntPtr, byte*, ulong, void> FxBurst;
        public delegate* unmanaged[Cdecl]<IntPtr, int, byte*, byte*, double, void> Ui;
        public delegate* unmanaged[Cdecl]<IntPtr, int, uint> Buttons;
        public delegate* unmanaged[Cdecl]<IntPtr, byte*, byte*, byte*, byte*, int, int> PluginCall;
        // ABI 3: localisation.
        public delegate* unmanaged[Cdecl]<IntPtr, byte*, byte*, byte*, int, int> Translate;
        public delegate* unmanaged[Cdecl]<IntPtr, byte*, int> LangSet;
        public delegate* unmanaged[Cdecl]<IntPtr, byte*, int, int> LangGet;
    }

    internal static unsafe class Utf8
    {
        // NUL-terminated UTF-8.
        public static byte[] Z(string s) => System.Text.Encoding.UTF8.GetBytes((s ?? "") + "\0");
        public static string Read(byte* p) => p == null ? "" : Marshal.PtrToStringUTF8((IntPtr)p) ?? "";
        public static string Read(byte* p, int len) =>
            p == null || len <= 0 ? "" : System.Text.Encoding.UTF8.GetString(p, len);
    }
}
