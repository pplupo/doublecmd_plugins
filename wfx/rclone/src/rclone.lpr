library rclone;

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

{$mode delphi}{$H+}

uses
{$IFDEF UNIX}
  cthreads,
{$ENDIF}
  Classes, SysUtils,
  uRcloneFunc, uRcloneCli, uRcloneJson, uRcloneUtil;

exports
  { WFX API }
  FsInitW,
  FsFindFirstW,
  FsFindNextW,
  FsFindClose,
  FsGetFileW,
  FsPutFileW,
  FsDeleteFileW,
  FsRemoveDirW,
  FsMkDirW,
  FsRenMovFileW,
  FsExecuteFileW,
  FsDisconnectW,
  FsGetDefRootName,
  FsSetDefaultParams,
  FsGetBackgroundFlags,
  FsExtractCustomIconW,
  { Extension API }
  ExtensionInitialize;

{$R *.res}

begin
  { A plugin is a shared library with its own RTL instance, so it gets the RTL's
    default code page (CP_ACP) rather than the host's. Combined with the fallback
    widestring manager (no cwstring), every UnicodeString -> AnsiString conversion
    would then replace code points above U+007F with '?' - which is what fpjson does
    internally while scanning the JSON that rclone prints. Declaring UTF-8 up front
    keeps non-ASCII file names intact on the way in and out. }
  DefaultSystemCodePage := CP_UTF8;
  DefaultUnicodeCodePage := CP_UTF8;
  DefaultFileSystemCodePage := CP_UTF8;
  DefaultRTLFileSystemCodePage := CP_UTF8;
end.
