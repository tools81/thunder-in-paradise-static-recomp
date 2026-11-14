; Inno Setup script for the Thunder in Paradise PC port (compiled by tools/package.ps1).
;
; The program is portable: everything it writes (saves, quicksaves,
; settings, the disc location, the log) goes into the folder it is installed
; to, never to AppData. So the default folder is a per-user one that needs no
; administrator rights (%LOCALAPPDATA%\Programs\Thunder in Paradise), and the
; folder can be moved or copied elsewhere afterwards.
;
; During setup the player points it at their own copy of the game (the
; folder holding the CD-i .cue/.bin, e.g. a "rom" folder). The choice is
; written to saves\disc_path.txt, which the game reads at start-up.

#ifndef AppVersion
  #define AppVersion "1.0.0"
#endif
#ifndef DistDir
  #define DistDir "..\dist\stage"
#endif
#ifndef OutDir
  #define OutDir "..\dist"
#endif

[Setup]
AppId={{B7E2D1F4-5A3C-4C8E-9F21-7D4A6E0B3C95}
AppName=Thunder in Paradise (CD-i PC port)
AppVersion={#AppVersion}
AppVerName=Thunder in Paradise (CD-i PC port) {#AppVersion}
AppPublisher=thunder-in-paradise-static-recomp
AppPublisherURL=https://github.com/tools81/thunder-in-paradise-static-recomp
AppUpdatesURL=https://github.com/tools81/thunder-in-paradise-static-recomp/releases
DefaultDirName={localappdata}\Programs\Thunder in Paradise
DefaultGroupName=Thunder in Paradise
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
OutputDir={#OutDir}
OutputBaseFilename=ThunderInParadise-{#AppVersion}-setup
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
UninstallDisplayIcon={app}\thunder.exe
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; GroupDescription: "Shortcuts:"

[Files]
Source: "{#DistDir}\thunder.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#DistDir}\README.txt"; DestDir: "{app}"; Flags: ignoreversion isreadme
Source: "{#DistDir}\THIRD-PARTY.txt"; DestDir: "{app}"; Flags: ignoreversion

[Dirs]
Name: "{app}\saves"

[Icons]
; --portable is the default behaviour; it is passed so the intent is visible in the shortcut
Name: "{group}\Thunder in Paradise"; Filename: "{app}\thunder.exe"; Parameters: "--portable"; WorkingDir: "{app}"
Name: "{group}\Read me"; Filename: "{app}\README.txt"
Name: "{group}\Uninstall Thunder in Paradise"; Filename: "{uninstallexe}"
Name: "{autodesktop}\Thunder in Paradise"; Filename: "{app}\thunder.exe"; Parameters: "--portable"; WorkingDir: "{app}"; Tasks: desktopicon

[Run]
Filename: "{app}\thunder.exe"; Parameters: "--portable"; WorkingDir: "{app}"; Description: "Play Thunder in Paradise"; Flags: postinstall nowait skipifsilent unchecked

[UninstallDelete]
; files the program made itself; saves\ (saves, quicksaves, settings) is left for the player to keep or delete
Type: files; Name: "{app}\saves\recomp.log"
Type: files; Name: "{app}\saves\ramdump.bin"
Type: filesandordirs; Name: "{app}\saves\update"

[Code]
var
  DiscPage: TInputDirWizardPage;

function FindImage(Dir: String; var Image: String): Boolean;
var
  R: TFindRec;
begin
  Result := False;
  if FindFirst(AddBackslash(Dir) + '*.cue', R) then begin
    Image := AddBackslash(Dir) + R.Name; Result := True; FindClose(R); exit;
  end;
  if FindFirst(AddBackslash(Dir) + '*.bin', R) then begin
    Image := AddBackslash(Dir) + R.Name; Result := True; FindClose(R);
  end;
end;

procedure InitializeWizard;
begin
  DiscPage := CreateInputDirPage(wpSelectDir,
    'Game disc', 'Where are your Thunder in Paradise game files?',
    'Thunder in Paradise runs from your own copy of the Philips CD-i disc: a disc image in ' +
    '.cue/.bin form (for example "Thunder in Paradise (USA) (Disc 1) (The Game).cue" and ".bin"). ' +
    'Select the folder that holds those files (often called "rom"), then click Next.' + #13#10#13#10 +
    'If the files are not on this PC yet, click Next anyway: the game asks for the disc the first ' +
    'time it starts.',
    False, '');
  DiscPage.Add('Folder with the .cue / .bin files:');
  { silent installs: setup.exe /VERYSILENT /DISC="D:\Games\Thunder" }
  DiscPage.Values[0] := ExpandConstant('{param:DISC|}');
  if DiscPage.Values[0] = '' then DiscPage.Values[0] := ExpandConstant('{src}');
end;

function ShouldSkipPage(PageID: Integer): Boolean;
begin
  Result := (PageID = DiscPage.ID) and WizardSilent;
end;

function NextButtonClick(CurPageID: Integer): Boolean;
var
  Image, Dir: String;
begin
  Result := True;
  if (CurPageID = DiscPage.ID) and (Trim(DiscPage.Values[0]) <> '') then begin
    Dir := Trim(DiscPage.Values[0]);
    if FileExists(Dir) then exit;   { a .cue/.bin typed directly }
    if not (FindImage(Dir, Image) or FindImage(AddBackslash(Dir) + 'rom', Image)) then
      Result := MsgBox('No .cue or .bin file was found in' + #13#10 + Dir + #13#10#13#10 +
                       'Continue anyway? (The game will ask for the disc when it starts.)',
                       mbConfirmation, MB_YESNO) = IDYES;
  end;
end;

{ only a folder (or file) that really holds the image is saved, so an update
  that is clicked through keeps the disc location already in saves\ }
function DiscChosen: Boolean;
var
  Image, Dir: String;
begin
  Dir := Trim(DiscPage.Values[0]);
  Result := (Dir <> '') and (FileExists(Dir) or FindImage(Dir, Image) or FindImage(AddBackslash(Dir) + 'rom', Image));
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if (CurStep = ssPostInstall) and DiscChosen then begin
    ForceDirectories(ExpandConstant('{app}\saves'));
    SaveStringToFile(ExpandConstant('{app}\saves\disc_path.txt'), Trim(DiscPage.Values[0]) + #13#10, False);
  end;
end;
