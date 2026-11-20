// Substitute AssemblyInfo for the FNA core built as Microsoft.Xna.Framework.
// Replaces FNA's own src/Properties/AssemblyInfo.cs (AssemblyVersion
// 26.11.0.0) at build time so the assembly binds to Celeste.exe's reference
// to Microsoft.Xna.Framework 4.0.0.0. The code is FNA's (MS-PL); only the
// assembly identity is retargeted. See third_party/fna/README.md.
using System;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

[assembly: AssemblyTitle("Microsoft.Xna.Framework")]
[assembly: AssemblyDescription("FNA (accuracy-focused XNA 4.0 reimplementation), built as Microsoft.Xna.Framework for Tiramisu Path B")]
[assembly: AssemblyConfiguration("Release")]
[assembly: AssemblyCompany("FNA project (Ethan Lee and contributors)")]
[assembly: AssemblyProduct("FNA")]
[assembly: AssemblyCopyright("Copyright (c) 2009-2024 Ethan Lee and FNA contributors")]
[assembly: AssemblyTrademark("")]
[assembly: AssemblyCulture("")]
[assembly: CLSCompliant(false)]
[assembly: ComVisible(false)]
[assembly: Guid("81119db2-82a6-45fb-a366-63a08437b485")]
[assembly: AssemblyVersion("4.0.0.0")]
[assembly: AssemblyFileVersion("4.0.0.0")]
