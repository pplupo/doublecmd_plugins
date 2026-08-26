{
   Double Commander
   -------------------------------------------------------------------------
   WFX plugin for working with rclone remotes

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

unit uRcloneUtil;

{$mode delphi}{$H+}

interface

uses
  SysUtils, WfxPlugin;

{ Path parsing functions }
function ExtractRemoteName(const Path: UnicodeString): UnicodeString;
function ExtractRemotePath(const Path: UnicodeString): UnicodeString;
function BuildRclonePath(const RemoteName, RemotePath: UnicodeString): UnicodeString;
function IsRootPath(const Path: UnicodeString): Boolean;

{ String conversion utilities }
function WideToUTF8(const S: UnicodeString): RawByteString;
function UTF8ToWide(const S: RawByteString): UnicodeString;
function StrToFileTime(const ISOTime: AnsiString): TFileTime;

{ File attribute helpers }
function UnixModeToAttributes(Mode: LongWord; IsDir: Boolean): DWORD;

implementation

uses
  DateUtils;

function ExtractRemoteName(const Path: UnicodeString): UnicodeString;
var
  I: Integer;
  WorkPath: UnicodeString;
begin
  Result := '';
  WorkPath := Path;

  // Remove leading slash
  if (Length(WorkPath) > 0) and (WorkPath[1] = '/') then
    Delete(WorkPath, 1, 1);
  if (Length(WorkPath) > 0) and (WorkPath[1] = '\') then
    Delete(WorkPath, 1, 1);

  // Find first path separator
  I := Pos('/', WorkPath);
  if I = 0 then
    I := Pos('\', WorkPath);

  if I > 0 then
    Result := Copy(WorkPath, 1, I - 1)
  else
    Result := WorkPath;
end;

function ExtractRemotePath(const Path: UnicodeString): UnicodeString;
var
  I: Integer;
  WorkPath: UnicodeString;
begin
  Result := '';
  WorkPath := Path;

  // Remove leading slash
  if (Length(WorkPath) > 0) and (WorkPath[1] = '/') then
    Delete(WorkPath, 1, 1);
  if (Length(WorkPath) > 0) and (WorkPath[1] = '\') then
    Delete(WorkPath, 1, 1);

  // Find first path separator (after remote name)
  I := Pos('/', WorkPath);
  if I = 0 then
    I := Pos('\', WorkPath);

  if I > 0 then
    Result := Copy(WorkPath, I + 1, MaxInt)
  else
    Result := '';

  // Convert backslashes to forward slashes for rclone. Done in place rather than with
  // StringReplace, which has no UnicodeString overload and would round-trip the path
  // through the system code page.
  for I := 1 to Length(Result) do
    if Result[I] = '\' then
      Result[I] := '/';
end;

function BuildRclonePath(const RemoteName, RemotePath: UnicodeString): UnicodeString;
begin
  if RemotePath = '' then
    Result := RemoteName + ':'
  else
    Result := RemoteName + ':' + RemotePath;
end;

function IsRootPath(const Path: UnicodeString): Boolean;
begin
  Result := (Path = '/') or (Path = '\') or (Path = '');
end;

{ Convert UTF-16 to UTF-8 bytes.

  Uses the System unit primitives rather than UTF8Encode/UTF8Decode on purpose: those
  go through the widestring manager, which without cwstring maps every code point above
  U+007F to '?'. RawByteString (CP_NONE) keeps the compiler from inserting an implicit
  code page conversion at the call sites. }
function WideToUTF8(const S: UnicodeString): RawByteString;
var
  Len: SizeUInt;
begin
  Result := '';
  if S = '' then Exit;
  // Worst case is 3 bytes per UTF-16 code unit, plus the terminating #0
  SetLength(Result, Length(S) * 3 + 1);
  Len := System.UnicodeToUtf8(PAnsiChar(Result), Length(Result), PUnicodeChar(S), Length(S));
  if Len > 1 then
    SetLength(Result, Len - 1)  // Len counts the terminating #0
  else
    Result := '';
end;

{ Convert UTF-8 bytes to UTF-16. See the note on WideToUTF8. }
function UTF8ToWide(const S: RawByteString): UnicodeString;
var
  Len: SizeUInt;
begin
  Result := '';
  if S = '' then Exit;
  // UTF-8 never expands: at most one UTF-16 code unit per byte, plus the terminating #0
  SetLength(Result, Length(S) + 1);
  Len := System.Utf8ToUnicode(PUnicodeChar(Result), Length(Result), PAnsiChar(S), Length(S));
  if Len > 1 then
    SetLength(Result, Len - 1)  // Len counts the terminating #0
  else
    Result := '';
end;

function StrToFileTime(const ISOTime: AnsiString): TFileTime;
var
  DT: TDateTime;
  Year, Month, Day, Hour, Min, Sec: Integer;
  MilliSec: Integer;
  Ticks: Int64;
begin
  Result.dwLowDateTime := $FFFFFFFE;
  Result.dwHighDateTime := $FFFFFFFF;

  if Length(ISOTime) < 19 then
    Exit;

  try
    // Parse ISO 8601 format: 2024-01-15T10:30:00.000000000Z
    Year := StrToIntDef(Copy(ISOTime, 1, 4), 0);
    Month := StrToIntDef(Copy(ISOTime, 6, 2), 0);
    Day := StrToIntDef(Copy(ISOTime, 9, 2), 0);
    Hour := StrToIntDef(Copy(ISOTime, 12, 2), 0);
    Min := StrToIntDef(Copy(ISOTime, 15, 2), 0);
    Sec := StrToIntDef(Copy(ISOTime, 18, 2), 0);

    // Parse milliseconds if present
    MilliSec := 0;
    if (Length(ISOTime) > 20) and (ISOTime[20] = '.') then
    begin
      // Take first 3 digits after decimal point for milliseconds
      MilliSec := StrToIntDef(Copy(ISOTime, 21, 3), 0);
    end;

    if (Year = 0) or (Month = 0) or (Day = 0) then
      Exit;

    DT := EncodeDateTime(Year, Month, Day, Hour, Min, Sec, MilliSec);

    // Convert to Windows FILETIME (100-nanosecond intervals since Jan 1, 1601)
    // TDateTime is days since Dec 30, 1899
    // Difference between 1601 and 1899 is 109205 days
    Ticks := Round((DT + 109205) * 864000000000.0);

    Result.dwLowDateTime := DWORD(Ticks);
    Result.dwHighDateTime := DWORD(Ticks shr 32);
  except
    // Return invalid time on parse error
  end;
end;

function UnixModeToAttributes(Mode: LongWord; IsDir: Boolean): DWORD;
begin
  if IsDir then
    Result := FILE_ATTRIBUTE_DIRECTORY
  else
    Result := FILE_ATTRIBUTE_NORMAL;

  // Store Unix mode in high bits
  Result := Result or FILE_ATTRIBUTE_UNIX_MODE or (Mode shl 16);
end;

end.
