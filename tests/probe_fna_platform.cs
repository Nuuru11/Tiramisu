// tiramisu/tests/probe_fna_platform.cs
// Headless probe for the Tiramisu_FNAPlatform backend (Path B step 2).
// Runs under embedded Mono (see tests/test_fna_platform.cpp, which feeds
// input events into the native ring before the run and a mouse delta
// mid-run). The probe:
//   1. checks all 56 FNAPlatform delegate fields are populated AND point
//      at Tiramisu_FNAPlatform (backend selection proof),
//   2. exercises the headless-safe subset: window/display queries,
//      mouse state round-trips, gamepad disconnected answers, storage,
//      file reads, scancode mapping, text-input state, and the event
//      path (PollEvents drains the fed key/wheel/char/click events into
//      Keyboard/Mouse/TextInput state).
// No graphics device is created (headless has no surface for FNA3D).
// Exit 0 = every check passed; exit 1 = any failure (named on stdout).
using System;
using System.IO;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Threading;
using Microsoft.Xna.Framework;
using Microsoft.Xna.Framework.Graphics;
using Microsoft.Xna.Framework.Input;

class ProbeFNAPlatform
{
	static int failures = 0;
	static int checks = 0;

	static Type PlatformType;

	static void Check(bool cond, string name)
	{
		checks += 1;
		if (cond)
		{
			Console.WriteLine("CHECK-OK " + name);
		}
		else
		{
			failures += 1;
			Console.WriteLine("CHECK-FAIL " + name);
		}
	}

	static object Invoke(string name, params object[] args)
	{
		FieldInfo f = PlatformType.GetField(
			name, BindingFlags.Public | BindingFlags.Static);
		if (f == null)
		{
			throw new MissingFieldException("FNAPlatform." + name);
		}
		Delegate d = (Delegate) f.GetValue(null);
		if (d == null)
		{
			throw new NullReferenceException("FNAPlatform." + name + " is null");
		}
		return d.DynamicInvoke(args);
	}

	static string[] DelegateNames = new string[]
	{
		"Malloc", "Free", "SetEnv",
		"CreateWindow", "DisposeWindow", "ApplyWindowChanges",
		"ScaleForWindow", "GetWindowBounds",
		"GetWindowResizable", "SetWindowResizable",
		"GetWindowBorderless", "SetWindowBorderless",
		"SetWindowTitle", "IsScreenKeyboardShown",
		"RegisterGame", "UnregisterGame", "PollEvents",
		"GetGraphicsAdapters", "GetCurrentDisplayMode", "GetMonitorHandle",
		"GetKeyFromScancode",
		"IsTextInputActive", "StartTextInput", "StopTextInput",
		"SetTextInputRectangle",
		"GetMouseState", "SetMousePosition", "OnIsMouseVisibleChanged",
		"GetRelativeMouseMode", "SetRelativeMouseMode",
		"GetGamePadCapabilities", "GetGamePadState",
		"SetGamePadVibration", "SetGamePadTriggerVibration",
		"GetGamePadGUID", "SetGamePadLightBar",
		"GetGamePadGyro", "GetGamePadAccelerometer",
		"GetStorageRoot", "GetDriveInfo",
		"ReadFileToPointer", "FreeFilePointer",
		"ShowRuntimeError",
		"GetMicrophones", "GetMicrophoneSamples",
		"GetMicrophoneQueuedBytes", "StartMicrophone", "StopMicrophone",
		"GetTouchCapabilities", "UpdateTouchPanelState",
		"GetNumTouchFingers",
		"SupportsOrientationChanges",
		"NeedsPlatformMainLoop", "RunPlatformMainLoop",
		"WrapWindow", "UnwrapWindow",
	};

	static char gotChar = (char) 0;
	static int gotClick = -1;

	static void OnText(char c)
	{
		gotChar = c;
	}

	static void OnClick(int button)
	{
		gotClick = button;
	}

	static int Main()
	{
		Assembly fna = Assembly.Load(
			"Microsoft.Xna.Framework, Version=4.0.0.0, Culture=neutral, " +
			"PublicKeyToken=842cf8be1de50553");
		PlatformType = fna.GetType("Microsoft.Xna.Framework.FNAPlatform");
		if (PlatformType == null)
		{
			Console.WriteLine("CHECK-FAIL FNAPlatform type not found");
			return 1;
		}

		// --- 1. function table: all 56 populated, Tiramisu backend ---
		int populated = 0;
		string backend = null;
		foreach (string name in DelegateNames)
		{
			FieldInfo f = PlatformType.GetField(
				name, BindingFlags.Public | BindingFlags.Static);
			if (f == null)
			{
				Console.WriteLine("TABLE-FIELD-MISSING " + name);
				failures += 1;
				continue;
			}
			Delegate d = (Delegate) f.GetValue(null);
			if (d == null)
			{
				Console.WriteLine("TABLE-DELEGATE-NULL " + name);
				failures += 1;
				continue;
			}
			string decl = d.Method.DeclaringType.FullName;
			if (backend == null)
			{
				backend = decl;
			}
			if (decl != "Microsoft.Xna.Framework.Tiramisu_FNAPlatform")
			{
				Console.WriteLine("TABLE-WRONG-BACKEND " + name + " -> " + decl);
				failures += 1;
				continue;
			}
			populated += 1;
		}
		Console.WriteLine("TABLE " + populated + "/" + DelegateNames.Length +
			" populated, backend=" + backend);
		Check(populated == 56, "table-56-populated");
		Check(backend == "Microsoft.Xna.Framework.Tiramisu_FNAPlatform",
			"table-backend-is-tiramisu");

		// --- 2. window + display ---
		GameWindow window = (GameWindow) Invoke("CreateWindow");
		Check(window != null, "window-created");
		Check(window.Handle != IntPtr.Zero, "window-handle-nonzero");

		GraphicsAdapter[] adapters =
			(GraphicsAdapter[]) Invoke("GetGraphicsAdapters");
		Check(adapters != null && adapters.Length == 1, "adapters-single");
		Check(adapters[0].DeviceName == @"\\.\\DISPLAY1",
			"adapter-device-name");
		DisplayModeCollection modes = adapters[0].SupportedDisplayModes;
		int modeCount = 0;
		DisplayMode firstMode = null;
		foreach (DisplayMode m in modes)
		{
			if (modeCount == 0)
			{
				firstMode = m;
			}
			modeCount++;
		}
		Check(modeCount >= 1 && firstMode != null &&
			firstMode.Width == 1920 && firstMode.Height == 1080,
			"adapter-mode-1920x1080");

		DisplayMode current =
			(DisplayMode) Invoke("GetCurrentDisplayMode", 0);
		Check(current.Width == 1920 && current.Height == 1080,
			"current-mode-1920x1080");

		IntPtr mon = (IntPtr) Invoke("GetMonitorHandle", 0);
		Check(mon == IntPtr.Zero, "monitor-handle-0");

		Rectangle bounds =
			(Rectangle) Invoke("GetWindowBounds", window.Handle);
		Check(bounds.X == 0 && bounds.Y == 0 && bounds.Width == 1920 &&
			bounds.Height == 1080, "window-bounds-1920x1080");

		Check((bool) Invoke("GetWindowResizable", window.Handle) == false,
			"window-not-resizable");
		Invoke("SetWindowResizable", window.Handle, true);
		Check(true, "set-window-resizable-no-throw");
		Check((bool) Invoke("GetWindowBorderless", window.Handle) == true,
			"window-borderless");
		Invoke("SetWindowBorderless", window.Handle, false);
		Check(true, "set-window-borderless-no-throw");
		Invoke("SetWindowTitle", window.Handle, "probe");
		Check(true, "set-window-title-no-throw");

		object[] scaleArgs = new object[] { window.Handle, false, 800, 600 };
		Invoke("ScaleForWindow", scaleArgs);
		Check((int) scaleArgs[2] == 800 && (int) scaleArgs[3] == 600,
			"scale-for-window-noop");

		Check((bool) Invoke("IsScreenKeyboardShown", window.Handle) == false,
			"screen-keyboard-hidden");

		Check((IntPtr) Invoke("WrapWindow", window.Handle) == window.Handle,
			"wrap-window-identity");
		Check((IntPtr) Invoke("UnwrapWindow", window.Handle) == window.Handle,
			"unwrap-window-identity");

		GraphicsAdapter reg =
			(GraphicsAdapter) Invoke("RegisterGame", (Game) null);
		Check(reg != null && reg.DeviceName == @"\\.\\DISPLAY1",
			"register-game-adapter-0");
		Invoke("UnregisterGame", (Game) null);
		Check(true, "unregister-game-no-throw");

		Check((bool) Invoke("NeedsPlatformMainLoop") == false,
			"no-platform-main-loop");
		Invoke("RunPlatformMainLoop", (Game) null);
		Check(true, "run-platform-main-loop-no-throw");
		Check((bool) Invoke("SupportsOrientationChanges") == false,
			"no-orientation-changes");

		// --- 3. touch / microphone: genuinely absent on console ---
		Microsoft.Xna.Framework.Input.Touch.TouchPanelCapabilities caps =
			(Microsoft.Xna.Framework.Input.Touch.TouchPanelCapabilities)
				Invoke("GetTouchCapabilities");
		Check(caps.IsConnected == false, "touch-not-connected");
		Check((int) Invoke("GetNumTouchFingers") == 0, "touch-fingers-0");
		Invoke("UpdateTouchPanelState");
		Check(true, "update-touch-no-throw");

		Microsoft.Xna.Framework.Audio.Microphone[] mics =
			(Microsoft.Xna.Framework.Audio.Microphone[])
				Invoke("GetMicrophones");
		Check(mics != null && mics.Length == 0, "microphones-empty");
		Check((int) Invoke("GetMicrophoneSamples", IntPtr.Zero,
				new byte[16], 0, 0) == 0, "mic-samples-0");
		Check((int) Invoke("GetMicrophoneQueuedBytes", IntPtr.Zero) == 0,
			"mic-queued-0");
		Invoke("StartMicrophone", IntPtr.Zero);
		Invoke("StopMicrophone", IntPtr.Zero);
		Check(true, "mic-start-stop-no-throw");

		// --- 4. gamepad: disconnected headless ---
		GamePadCapabilities gcaps =
			(GamePadCapabilities) Invoke("GetGamePadCapabilities", 0);
		Check(gcaps.IsConnected == false, "pad0-disconnected-caps");
		GamePadState gstate = (GamePadState) Invoke("GetGamePadState", 0,
			GamePadDeadZone.IndependentAxes);
		Check(gstate.IsConnected == false, "pad0-disconnected-state");
		Check((bool) Invoke("SetGamePadVibration", 0, 1.0f, 1.0f) == false,
			"pad0-vibration-false");
		Check((bool) Invoke("SetGamePadTriggerVibration", 0, 1.0f, 1.0f)
			== false, "pad0-trigger-vibration-false");
		Check((string) Invoke("GetGamePadGUID", 0) == String.Empty,
			"pad0-guid-empty");
		object[] gyroArgs = new object[] { 0, new Vector3() };
		Check((bool) Invoke("GetGamePadGyro", gyroArgs) == false,
			"pad0-gyro-false");
		object[] accelArgs = new object[] { 0, new Vector3() };
		Check((bool) Invoke("GetGamePadAccelerometer", accelArgs) == false,
			"pad0-accel-false");
		Invoke("SetGamePadLightBar", 0, Color.Red);
		Check(true, "pad0-lightbar-no-throw");

		// --- 5. event path: drain the fed key/wheel/char/click ---
		TextInputEXT.TextInput += OnText;
		Mouse.ClickedEXT += OnClick;
		TextInputEXT.StartTextInput();
		Check(TextInputEXT.IsTextInputActive(), "text-input-active");

		object[] pollArgs = new object[]
			{ null, null, new bool[7], false };
		Invoke("PollEvents", pollArgs);

		Keys[] pressed = Keyboard.GetState().GetPressedKeys();
		bool sawA = false;
		foreach (Keys k in pressed)
		{
			if (k == Keys.A)
			{
				sawA = true;
			}
		}
		Check(sawA, "event-key-A-pressed");

		MouseState ms = Mouse.GetState();
		Check(ms.ScrollWheelValue == 120, "event-wheel-120");
		Check(gotChar == 'x', "event-char-x");
		Check(gotClick == 0, "event-click-left");

		TextInputEXT.StopTextInput();
		Check(!TextInputEXT.IsTextInputActive(), "text-input-stopped");
		Invoke("SetTextInputRectangle", window.Handle,
			new Rectangle(0, 0, 10, 10));
		Check(true, "set-text-input-rect-no-throw");

		// --- 6. mouse state: fed position/buttons + round-trip ---
		object[] margs = new object[]
		{
			window.Handle, 0, 0,
			ButtonState.Released, ButtonState.Released,
			ButtonState.Released, ButtonState.Released,
			ButtonState.Released
		};
		Invoke("GetMouseState", margs);
		Check((int) margs[1] == 100 && (int) margs[2] == 200,
			"mouse-fed-position-100-200");
		Check((ButtonState) margs[3] == ButtonState.Pressed,
			"mouse-fed-left-pressed");
		Check((ButtonState) margs[4] == ButtonState.Released &&
			(ButtonState) margs[5] == ButtonState.Released &&
			(ButtonState) margs[6] == ButtonState.Released &&
			(ButtonState) margs[7] == ButtonState.Released,
			"mouse-fed-others-released");

		Invoke("SetMousePosition", window.Handle, 300, 400);
		Invoke("GetMouseState", margs);
		Check((int) margs[1] == 300 && (int) margs[2] == 400,
			"mouse-set-position-roundtrip");

		Invoke("OnIsMouseVisibleChanged", true);
		Invoke("OnIsMouseVisibleChanged", false);
		Check(true, "mouse-visible-toggle-no-throw");

		// --- 7. relative mouse: drain, then the mid-run fed delta ---
		Check((bool) Invoke("GetRelativeMouseMode", window.Handle) == false,
			"relative-mode-default-false");
		Invoke("SetRelativeMouseMode", window.Handle, true);
		Check((bool) Invoke("GetRelativeMouseMode", window.Handle) == true,
			"relative-mode-enabled");

		/* The driver feeds a (7,-4) raw delta ~3s into the run (and
		 * again at ~5s). Poll until it arrives; each read consumes.
		 */
		bool sawDelta = false;
		for (int i = 0; i < 80 && !sawDelta; i += 1)
		{
			Invoke("GetMouseState", margs);
			if ((int) margs[1] == 7 && (int) margs[2] == -4)
			{
				sawDelta = true;
			}
			else
			{
				Thread.Sleep(100);
			}
		}
		Check(sawDelta, "relative-delta-7-minus-4");
		Invoke("GetMouseState", margs);
		Check((int) margs[1] == 0 && (int) margs[2] == 0,
			"relative-delta-consumed");
		Invoke("SetRelativeMouseMode", window.Handle, false);
		Check((bool) Invoke("GetRelativeMouseMode", window.Handle) == false,
			"relative-mode-disabled");

		// --- 8. memory / env / storage / files / keys ---
		IntPtr mem = (IntPtr) Invoke("Malloc", 64);
		Check(mem != IntPtr.Zero, "malloc-nonzero");
		Invoke("Free", mem);
		Check(true, "free-no-throw");

		Invoke("SetEnv", "TIRAMISU_FNA_PROBE_X", "42");
		Check(Environment.GetEnvironmentVariable("TIRAMISU_FNA_PROBE_X")
			== "42", "setenv-roundtrip");

		string root = (string) Invoke("GetStorageRoot");
		Check(!String.IsNullOrEmpty(root) &&
			root.EndsWith("probe_fna_platform"),
			"storage-root-exe-name");
		DriveInfo drive = (DriveInfo) Invoke("GetDriveInfo", root);
		Check(drive != null, "drive-info-nonnull");

		string tmp = Path.GetTempFileName();
		byte[] content = new byte[] { 1, 2, 3, 4, 5, 6, 7, 8 };
		File.WriteAllBytes(tmp, content);
		object[] fileArgs = new object[] { tmp, IntPtr.Zero };
		IntPtr filePtr = (IntPtr) Invoke("ReadFileToPointer", fileArgs);
		IntPtr fileSize = (IntPtr) fileArgs[1];
		bool fileOk = filePtr != IntPtr.Zero && fileSize.ToInt64() == 8;
		if (fileOk)
		{
			byte[] back = new byte[8];
			Marshal.Copy(filePtr, back, 0, 8);
			for (int i = 0; i < 8; i += 1)
			{
				if (back[i] != content[i])
				{
					fileOk = false;
				}
			}
			Invoke("FreeFilePointer", filePtr);
		}
		Check(fileOk, "read-file-roundtrip");
		File.Delete(tmp);
		object[] missingArgs = new object[]
			{ "/nonexistent-tiramisu-probe-file.bin", IntPtr.Zero };
		IntPtr missingPtr = (IntPtr) Invoke("ReadFileToPointer", missingArgs);
		Check(missingPtr == IntPtr.Zero, "read-file-missing-null");

		Check((Keys) Invoke("GetKeyFromScancode", (Keys) 0x39) == Keys.Space,
			"scancode-0x39-space");
		Check((Keys) Invoke("GetKeyFromScancode", (Keys) 0x01) == Keys.Escape,
			"scancode-0x01-escape");
		Check((Keys) Invoke("GetKeyFromScancode", (Keys) 0x7F) == Keys.None,
			"scancode-0x7f-none");

		Invoke("ShowRuntimeError", window, "probe runtime error (expected)");
		Check(true, "show-runtime-error-no-throw");

		Invoke("DisposeWindow", window);
		Check(true, "dispose-window-no-throw");

		Console.WriteLine("PROBE " + (checks - failures) + "/" + checks +
			" checks passed, failures=" + failures);
		if (failures == 0)
		{
			Console.WriteLine("FNA-PLATFORM-OK");
			return 0;
		}
		Console.WriteLine("FNA-PLATFORM-FAIL");
		return 1;
	}
}
