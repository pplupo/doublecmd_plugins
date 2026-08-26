{
   Double Commander
   -------------------------------------------------------------------------
   WFX plugin for working with rclone remotes - CLI wrapper

   Copyright (C) 2026 Miklos Mukka Szel <hello@miklos-szel.com>

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 2.1 of the License, or (at your option) any later version.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with this library; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA
}

unit uRcloneCli;

{$mode delphi}{$H+}

interface

uses
  Classes, SysUtils, Process, Pipes, WfxPlugin, uRcloneJson;

type
  TProgressCallback = function(SourceName, TargetName: PWideChar;
    PercentDone: Integer): Integer;

  TRcloneCli = class
  private
    FRclonePath: AnsiString;
    FLastError: RawByteString;
    FProgressProc: TProgressCallback;
    FPluginNr: Integer;
    function RunCommand(const Args: array of RawByteString; out Output: RawByteString;
      out ErrorOutput: RawByteString; out ExitCode: Integer): Boolean;
    function RunCommandWithProgress(const Args: array of RawByteString;
      const SourceName, TargetName: UnicodeString;
      out ExitCode: Integer): Boolean;
    function ParseProgressPercentage(const Line: AnsiString): Integer;
  public
    constructor Create;
    destructor Destroy; override;

    { Initialize with callback functions }
    procedure Initialize(PluginNr: Integer; ProgressProc: TProgressCallback);

    { List all configured remotes. Returns nil if rclone failed; an empty list
      means rclone succeeded and there are no remotes. LastError has the details. }
    function ListRemotes: TStringList;

    { List directory contents. Returns nil if rclone failed; an empty list means
      rclone succeeded and the directory is empty. LastError has the details. }
    function ListDirectory(const RemotePath: RawByteString): TRcloneFileList;

    { File operations }
    function CopyToLocal(const RemotePath, LocalPath: RawByteString): Integer;
    function CopyToRemote(const LocalPath, RemotePath: RawByteString): Integer;
    function DeleteFile(const RemotePath: RawByteString): Boolean;
    function DeleteDir(const RemotePath: RawByteString): Boolean;
    function MakeDir(const RemotePath: RawByteString): Boolean;
    function MoveFile(const OldPath, NewPath: RawByteString): Boolean;

    property RclonePath: AnsiString read FRclonePath write FRclonePath;
    property LastError: RawByteString read FLastError;
  end;

{ Map rclone exit codes to WFX results }
function RcloneExitToWfx(ExitCode: Integer; const ErrorOutput: RawByteString): Integer;

implementation

uses
  uRcloneUtil;

const
  // Common rclone installation paths on different platforms
  RCLONE_SEARCH_PATHS: array[0..7] of AnsiString = (
    '/usr/local/bin/rclone',        // Homebrew on Intel Mac, manual install
    '/opt/homebrew/bin/rclone',     // Homebrew on Apple Silicon
    '/usr/bin/rclone',              // System package managers
    '/snap/bin/rclone',             // Snap on Linux
    '/home/linuxbrew/.linuxbrew/bin/rclone', // Linuxbrew
    'C:\Program Files\rclone\rclone.exe',    // Windows
    'C:\rclone\rclone.exe',         // Windows alternative
    'rclone'                        // Fallback to PATH
  );

function FindRclonePath: AnsiString;
var
  I: Integer;
begin
  // Search common paths
  for I := Low(RCLONE_SEARCH_PATHS) to High(RCLONE_SEARCH_PATHS) - 1 do
  begin
    if FileExists(RCLONE_SEARCH_PATHS[I]) then
    begin
      Result := RCLONE_SEARCH_PATHS[I];
      Exit;
    end;
  end;
  // Fallback to just 'rclone' and hope it's in PATH
  Result := 'rclone';
end;

constructor TRcloneCli.Create;
begin
  inherited Create;
  FRclonePath := FindRclonePath;
  FLastError := '';
  FProgressProc := nil;
  FPluginNr := 0;
end;

destructor TRcloneCli.Destroy;
begin
  inherited Destroy;
end;

procedure TRcloneCli.Initialize(PluginNr: Integer; ProgressProc: TProgressCallback);
begin
  FPluginNr := PluginNr;
  FProgressProc := ProgressProc;
end;

function GetHomeDir: AnsiString;
begin
  Result := GetEnvironmentVariable('HOME');
  if Result = '' then
    Result := ExpandFileName('~');
end;

function GetRcloneConfigPath: AnsiString;
var
  ConfigDir: AnsiString;
begin
  // Check XDG config first
  ConfigDir := GetEnvironmentVariable('XDG_CONFIG_HOME');
  if ConfigDir <> '' then
  begin
    Result := ConfigDir + '/rclone/rclone.conf';
    if FileExists(Result) then Exit;
  end;

  // Check ~/.config/rclone/rclone.conf
  Result := GetHomeDir + '/.config/rclone/rclone.conf';
  if FileExists(Result) then Exit;

  // Check ~/.rclone.conf (old location)
  Result := GetHomeDir + '/.rclone.conf';
  if FileExists(Result) then Exit;

  // Default to standard location
  Result := GetHomeDir + '/.config/rclone/rclone.conf';
end;

{ Read everything currently buffered in a pipe without blocking.
  Returns the number of bytes appended to AStream. }
function DrainPipe(APipe: TInputPipeStream; AStream: TStream): Integer;
var
  Buffer: array[0..4095] of Byte;
  Avail, BytesRead: Integer;
begin
  Result := 0;
  if APipe = nil then Exit;
  repeat
    Avail := APipe.NumBytesAvailable;
    if Avail <= 0 then Break;
    if Avail > SizeOf(Buffer) then
      Avail := SizeOf(Buffer);
    BytesRead := APipe.Read(Buffer, Avail);
    if BytesRead <= 0 then Break;
    AStream.Write(Buffer, BytesRead);
    Inc(Result, BytesRead);
  until False;
end;

{ Materialize a stream's bytes verbatim. Deliberately not TStringStream.DataString,
  which decodes and re-encodes through TEncoding.Default and would corrupt UTF-8 file
  names whenever the system code page is not UTF-8. }
function StreamToRawBytes(AStream: TMemoryStream): RawByteString;
begin
  if AStream.Size = 0 then
    Result := ''
  else
    SetString(Result, PAnsiChar(AStream.Memory), AStream.Size);
end;

function TRcloneCli.RunCommand(const Args: array of RawByteString;
  out Output: RawByteString; out ErrorOutput: RawByteString; out ExitCode: Integer): Boolean;
var
  AProcess: TProcess;
  I: Integer;
  OutputStream, ErrorStream: TMemoryStream;
  ConfigPath: AnsiString;
begin
  Result := False;
  Output := '';
  ErrorOutput := '';
  ExitCode := -1;
  FLastError := '';

  AProcess := TProcess.Create(nil);
  OutputStream := TMemoryStream.Create;
  ErrorStream := TMemoryStream.Create;
  try
    AProcess.Executable := FRclonePath;

    // Add --config flag to explicitly specify config location
    ConfigPath := GetRcloneConfigPath;
    if FileExists(ConfigPath) then
    begin
      AProcess.Parameters.Add('--config');
      AProcess.Parameters.Add(ConfigPath);
    end;

    for I := Low(Args) to High(Args) do
      AProcess.Parameters.Add(Args[I]);

    // Set up proper environment with PATH
    AProcess.Environment.Add('PATH=/usr/local/bin:/opt/homebrew/bin:/usr/bin:/bin:/usr/sbin:/sbin');
    AProcess.Environment.Add('HOME=' + GetHomeDir);

    // Keep stderr separate: rclone writes log messages (e.g. the SFTP
    // "No host key validation is being performed" NOTICE) to stderr, and
    // merging them into stdout would corrupt the JSON of lsjson & friends.
    AProcess.Options := [poUsePipes, poNoConsole];

    try
      AProcess.Execute;

      // Drain both pipes until the process is gone and nothing is left
      repeat
        if DrainPipe(AProcess.Output, OutputStream) +
           DrainPipe(AProcess.Stderr, ErrorStream) = 0 then
        begin
          if not AProcess.Running then Break;
          Sleep(10);
        end;
      until False;

      ExitCode := AProcess.ExitCode;
      Output := StreamToRawBytes(OutputStream);
      ErrorOutput := StreamToRawBytes(ErrorStream);
      Result := True;
    except
      on E: Exception do
      begin
        FLastError := E.Message;
        Result := False;
      end;
    end;
  finally
    ErrorStream.Free;
    OutputStream.Free;
    AProcess.Free;
  end;
end;

function TRcloneCli.ParseProgressPercentage(const Line: AnsiString): Integer;
var
  PctPos: Integer;
  PctStr: AnsiString;
  I: Integer;
begin
  Result := -1;

  // Look for percentage in rclone progress output
  // Format: "Transferred: ... 42%, ..."
  PctPos := Pos('%', Line);
  if PctPos > 0 then
  begin
    // Walk backwards to find the start of the number
    I := PctPos - 1;
    while (I > 0) and (Line[I] in ['0'..'9']) do
      Dec(I);

    if I < PctPos - 1 then
    begin
      PctStr := Copy(Line, I + 1, PctPos - I - 1);
      Result := StrToIntDef(PctStr, -1);
      if Result > 100 then
        Result := 100;
    end;
  end;
end;

function TRcloneCli.RunCommandWithProgress(const Args: array of RawByteString;
  const SourceName, TargetName: UnicodeString;
  out ExitCode: Integer): Boolean;
var
  AProcess: TProcess;
  I: Integer;
  BytesRead: Integer;
  Buffer: array[0..4095] of Byte;
  OutputLine: AnsiString;
  Pct: Integer;
  UserAbort: Boolean;
  WSource, WTarget: UnicodeString;
  ConfigPath: AnsiString;
begin
  Result := False;
  ExitCode := -1;
  FLastError := '';
  UserAbort := False;

  WSource := SourceName;
  WTarget := TargetName;

  AProcess := TProcess.Create(nil);
  try
    AProcess.Executable := FRclonePath;

    // Add --config flag to explicitly specify config location
    ConfigPath := GetRcloneConfigPath;
    if FileExists(ConfigPath) then
    begin
      AProcess.Parameters.Add('--config');
      AProcess.Parameters.Add(ConfigPath);
    end;

    for I := Low(Args) to High(Args) do
      AProcess.Parameters.Add(Args[I]);

    // Add progress flags
    AProcess.Parameters.Add('--progress');
    AProcess.Parameters.Add('--stats');
    AProcess.Parameters.Add('1s');

    // Set up proper environment with PATH
    AProcess.Environment.Add('PATH=/usr/local/bin:/opt/homebrew/bin:/usr/bin:/bin:/usr/sbin:/sbin');
    AProcess.Environment.Add('HOME=' + GetHomeDir);

    AProcess.Options := [poUsePipes, poStderrToOutPut, poNoConsole];

    try
      AProcess.Execute;

      // Report initial progress
      if Assigned(FProgressProc) then
      begin
        if FProgressProc(PWideChar(WSource), PWideChar(WTarget), 0) <> 0 then
        begin
          AProcess.Terminate(1);
          UserAbort := True;
        end;
      end;

      OutputLine := '';

      // Read output and parse progress
      while AProcess.Running and not UserAbort do
      begin
        BytesRead := AProcess.Output.Read(Buffer, SizeOf(Buffer));
        if BytesRead > 0 then
        begin
          SetString(OutputLine, PAnsiChar(@Buffer[0]), BytesRead);

          // Parse progress percentage
          Pct := ParseProgressPercentage(OutputLine);
          if (Pct >= 0) and Assigned(FProgressProc) then
          begin
            if FProgressProc(PWideChar(WSource), PWideChar(WTarget), Pct) <> 0 then
            begin
              AProcess.Terminate(1);
              UserAbort := True;
            end;
          end;
        end
        else
          Sleep(50);
      end;

      // Wait for process to finish
      if not UserAbort then
      begin
        while AProcess.Running do
          Sleep(10);
      end;

      ExitCode := AProcess.ExitCode;

      if UserAbort then
        ExitCode := -2  // Special code for user abort
      else
        Result := True;

      // Report completion
      if Assigned(FProgressProc) and not UserAbort then
        FProgressProc(PWideChar(WSource), PWideChar(WTarget), 100);

    except
      on E: Exception do
      begin
        FLastError := E.Message;
        Result := False;
      end;
    end;
  finally
    AProcess.Free;
  end;
end;

function TRcloneCli.ListRemotes: TStringList;
var
  Output, ErrorOutput: RawByteString;
  ExitCode: Integer;
begin
  Result := nil;
  if RunCommand(['listremotes'], Output, ErrorOutput, ExitCode) and (ExitCode = 0) then
    Result := ParseListRemotes(Output)
  else
    FLastError := ErrorOutput;
end;

function TRcloneCli.ListDirectory(const RemotePath: RawByteString): TRcloneFileList;
var
  Output, ErrorOutput: RawByteString;
  ExitCode: Integer;
begin
  Result := nil;
  if RunCommand(['lsjson', RemotePath], Output, ErrorOutput, ExitCode) and (ExitCode = 0) then
    Result := ParseLsJson(Output)
  else
    FLastError := ErrorOutput;
end;

function TRcloneCli.CopyToLocal(const RemotePath, LocalPath: RawByteString): Integer;
var
  ExitCode: Integer;
begin
  if RunCommandWithProgress(['copyto', RemotePath, LocalPath],
    UTF8ToWide(RemotePath), UTF8ToWide(LocalPath), ExitCode) then
  begin
    if ExitCode = 0 then
      Result := FS_FILE_OK
    else if ExitCode = -2 then
      Result := FS_FILE_USERABORT
    else
      Result := RcloneExitToWfx(ExitCode, FLastError);
  end
  else
    Result := FS_FILE_READERROR;
end;

function TRcloneCli.CopyToRemote(const LocalPath, RemotePath: RawByteString): Integer;
var
  ExitCode: Integer;
begin
  if RunCommandWithProgress(['copyto', LocalPath, RemotePath],
    UTF8ToWide(LocalPath), UTF8ToWide(RemotePath), ExitCode) then
  begin
    if ExitCode = 0 then
      Result := FS_FILE_OK
    else if ExitCode = -2 then
      Result := FS_FILE_USERABORT
    else
      Result := RcloneExitToWfx(ExitCode, FLastError);
  end
  else
    Result := FS_FILE_WRITEERROR;
end;

function TRcloneCli.DeleteFile(const RemotePath: RawByteString): Boolean;
var
  Output, ErrorOutput: RawByteString;
  ExitCode: Integer;
begin
  Result := RunCommand(['delete', RemotePath], Output, ErrorOutput, ExitCode) and (ExitCode = 0);
  if not Result then
    FLastError := ErrorOutput;
end;

function TRcloneCli.DeleteDir(const RemotePath: RawByteString): Boolean;
var
  Output, ErrorOutput: RawByteString;
  ExitCode: Integer;
begin
  Result := RunCommand(['rmdir', RemotePath], Output, ErrorOutput, ExitCode) and (ExitCode = 0);
  if not Result then
    FLastError := ErrorOutput;
end;

function TRcloneCli.MakeDir(const RemotePath: RawByteString): Boolean;
var
  Output, ErrorOutput: RawByteString;
  ExitCode: Integer;
begin
  Result := RunCommand(['mkdir', RemotePath], Output, ErrorOutput, ExitCode) and (ExitCode = 0);
  if not Result then
    FLastError := ErrorOutput;
end;

function TRcloneCli.MoveFile(const OldPath, NewPath: RawByteString): Boolean;
var
  Output, ErrorOutput: RawByteString;
  ExitCode: Integer;
begin
  Result := RunCommand(['moveto', OldPath, NewPath], Output, ErrorOutput, ExitCode) and (ExitCode = 0);
  if not Result then
    FLastError := ErrorOutput;
end;

function RcloneExitToWfx(ExitCode: Integer; const ErrorOutput: RawByteString): Integer;
var
  LowerError: RawByteString;
begin
  case ExitCode of
    0: Result := FS_FILE_OK;
    3: Result := FS_FILE_NOTFOUND;  // directory not found
    4: Result := FS_FILE_NOTFOUND;  // file not found
  else
    begin
      LowerError := LowerCase(ErrorOutput);

      if Pos('permission denied', LowerError) > 0 then
        Result := FS_FILE_READERROR
      else if Pos('no space', LowerError) > 0 then
        Result := FS_FILE_WRITEERROR
      else if Pos('not found', LowerError) > 0 then
        Result := FS_FILE_NOTFOUND
      else if Pos('access denied', LowerError) > 0 then
        Result := FS_FILE_READERROR
      else
        Result := FS_FILE_READERROR;
    end;
  end;
end;

end.
