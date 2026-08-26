{
   Double Commander
   -------------------------------------------------------------------------
   WFX plugin for working with rclone remotes - JSON parsing

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

unit uRcloneJson;

{$mode delphi}{$H+}

interface

uses
  Classes, SysUtils, Contnrs, WfxPlugin;

type
  TRcloneFile = class
  public
    Name: UnicodeString;
    Size: Int64;
    ModTime: AnsiString;  // ISO 8601 format
    IsDir: Boolean;
    MimeType: AnsiString;
  end;

  TRcloneFileList = class(TObjectList)
  private
    function GetItem(Index: Integer): TRcloneFile;
  public
    property Items[Index: Integer]: TRcloneFile read GetItem; default;
  end;

{ Parse rclone lsjson output (raw UTF-8 bytes as printed by rclone) }
function ParseLsJson(const JsonOutput: RawByteString): TRcloneFileList;

{ Parse rclone listremotes output }
function ParseListRemotes(const Output: RawByteString): TStringList;

implementation

uses
  fpjson, jsonparser, uRcloneUtil;

function TRcloneFileList.GetItem(Index: Integer): TRcloneFile;
begin
  Result := TRcloneFile(inherited Items[Index]);
end;

function ParseLsJson(const JsonOutput: RawByteString): TRcloneFileList;
var
  JsonData: TJSONData;
  JsonArray: TJSONArray;
  JsonObj: TJSONObject;
  I, StartPos: Integer;
  RcloneFile: TRcloneFile;
  Text: RawByteString;
begin
  Result := TRcloneFileList.Create(True);

  Text := Trim(JsonOutput);
  if Text = '' then
    Exit;

  // Be tolerant of anything printed before the array (log lines, warnings)
  if Text[1] <> '[' then
  begin
    StartPos := Pos('[', Text);
    if StartPos = 0 then
      Exit;
    Text := Copy(Text, StartPos, Length(Text) - StartPos + 1);
  end;

  try
    JsonData := GetJSON(Text);
    try
      if not (JsonData is TJSONArray) then
        Exit;

      JsonArray := TJSONArray(JsonData);

      for I := 0 to JsonArray.Count - 1 do
      begin
        if not (JsonArray.Items[I] is TJSONObject) then
          Continue;

        JsonObj := TJSONObject(JsonArray.Items[I]);
        RcloneFile := TRcloneFile.Create;

        // Parse Name. The explicit TJSONStringType cast picks the byte-string
        // overload of Get, so the name arrives as UTF-8 and is decoded exactly once.
        if JsonObj.Find('Name') <> nil then
          RcloneFile.Name := UTF8ToWide(JsonObj.Get('Name', TJSONStringType('')));

        // Parse Size
        if JsonObj.Find('Size') <> nil then
          RcloneFile.Size := JsonObj.Get('Size', Int64(0));

        // Parse ModTime
        if JsonObj.Find('ModTime') <> nil then
          RcloneFile.ModTime := JsonObj.Get('ModTime', '');

        // Parse IsDir
        if JsonObj.Find('IsDir') <> nil then
          RcloneFile.IsDir := JsonObj.Get('IsDir', False);

        // Parse MimeType (optional)
        if JsonObj.Find('MimeType') <> nil then
          RcloneFile.MimeType := JsonObj.Get('MimeType', '');

        Result.Add(RcloneFile);
      end;
    finally
      JsonData.Free;
    end;
  except
    // On JSON parse error, return empty list
  end;
end;

function ParseListRemotes(const Output: RawByteString): TStringList;
var
  Lines: TStringList;
  I: Integer;
  RemoteName: RawByteString;
begin
  Result := TStringList.Create;
  Lines := TStringList.Create;
  try
    Lines.Text := Output;
    for I := 0 to Lines.Count - 1 do
    begin
      RemoteName := Trim(Lines[I]);
      // rclone listremotes outputs "remotename:" - anything without the
      // trailing colon is not a remote (e.g. a stray log line)
      if (Length(RemoteName) < 2) or (RemoteName[Length(RemoteName)] <> ':') then
        Continue;
      RemoteName := Copy(RemoteName, 1, Length(RemoteName) - 1);
      Result.Add(RemoteName);
    end;
  finally
    Lines.Free;
  end;
end;

end.
