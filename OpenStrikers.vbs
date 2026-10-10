' Double-click to open the launcher without a terminal window.
Option Explicit
Dim files, shell, root, launcher
Set files = CreateObject("Scripting.FileSystemObject")
Set shell = CreateObject("WScript.Shell")
root = files.GetParentFolderName(WScript.ScriptFullName)
launcher = root & "\build-audio\openstrikers-launcher.exe"
If Not files.FileExists(launcher) Then launcher = root & "\build\openstrikers-launcher.exe"
If files.FileExists(launcher) Then
    shell.Run Chr(34) & launcher & Chr(34), 1, False
Else
    MsgBox "Build OpenStrikers first. The launcher will appear next to openstrikers.exe in your build folder.", 48, "OpenStrikers"
End If
