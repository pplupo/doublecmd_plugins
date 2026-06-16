unit fTweakPacker;

{$mode objfpc}{$H+}

interface

uses
  Classes, SysUtils, Forms, Controls, Dialogs, StdCtrls, ExtCtrls, Grids,
  uPluginTypes, uDcXml;

type
  TfrmTweakPacker = class(TForm)
    sgExts: TStringGrid;
    pnlFlags: TPanel;
    lblFlags: TLabel;
    btnOk: TButton;
    btnCancel: TButton;
    procedure FormCreate(Sender: TObject);
    procedure btnOkClick(Sender: TObject);
  private
    FPath: string;
    FCheckBoxes: array[0..9] of TCheckBox;
    procedure BuildFlagCheckboxes;
    procedure LoadFromXml;
    function CurrentFlags: Integer;
  public
    function EditWcx(const Path: string): Boolean;
  end;

function ShowTweakPacker(const Path: string): Boolean;

implementation

{$R *.lfm}

uses uPluginTweak, uDcPaths;

function ShowTweakPacker(const Path: string): Boolean;
var
  F: TfrmTweakPacker;
begin
  F := TfrmTweakPacker.Create(nil);
  try
    Result := F.EditWcx(Path);
  finally
    F.Free;
  end;
end;

procedure TfrmTweakPacker.FormCreate(Sender: TObject);
begin
  sgExts.ColCount := 2;
  sgExts.RowCount := 1;
  sgExts.FixedRows := 1;
  sgExts.Cells[0, 0] := 'Extension';
  sgExts.Cells[1, 0] := 'Flags';
  BuildFlagCheckboxes;
end;

procedure TfrmTweakPacker.BuildFlagCheckboxes;
var
  I, TopPos: Integer;
begin
  TopPos := 24;
  for I := 0 to High(PK_CAP_NAMES) do
  begin
    FCheckBoxes[I] := TCheckBox.Create(Self);
    FCheckBoxes[I].Parent := pnlFlags;
    FCheckBoxes[I].Left := 8;
    FCheckBoxes[I].Top := TopPos;
    FCheckBoxes[I].Width := 400;
    FCheckBoxes[I].Caption := PK_CAP_NAMES[I].Name;
    FCheckBoxes[I].Tag := PK_CAP_NAMES[I].Flag;
    Inc(TopPos, 24);
  end;
end;

procedure TfrmTweakPacker.LoadFromXml;
var
  Doc: TDcXmlDocument;
  Entries: TPluginXmlEntryArray;
  I, J, Flags: Integer;
begin
  Doc := TDcXmlDocument.Create;
  try
    Doc.LoadFromFile(DcPaths.DoubleCmdXmlPath);
    Entries := Doc.EnumerateActive(ptWCX);
    sgExts.RowCount := 1;
    for I := 0 to High(Entries) do
      if SameText(DcPaths.ExpandDcPath(Entries[I].Path), DcPaths.ExpandDcPath(FPath)) then
      begin
        sgExts.RowCount := sgExts.RowCount + 1;
        sgExts.Cells[0, sgExts.RowCount - 1] := Entries[I].ArchiveExt;
        sgExts.Cells[1, sgExts.RowCount - 1] := IntToStr(Entries[I].Flags);
        if sgExts.RowCount = 2 then
        begin
          Flags := Entries[I].Flags;
          for J := 0 to High(FCheckBoxes) do
            FCheckBoxes[J].Checked := (Flags and FCheckBoxes[J].Tag) <> 0;
        end;
      end;
  finally
    Doc.Free;
  end;
end;

function TfrmTweakPacker.CurrentFlags: Integer;
var
  I: Integer;
begin
  Result := 0;
  for I := 0 to High(FCheckBoxes) do
    if FCheckBoxes[I].Checked then
      Result := Result or FCheckBoxes[I].Tag;
end;

function TfrmTweakPacker.EditWcx(const Path: string): Boolean;
var
  Exts: TStringList;
  FlagsArr: array of Integer;
  R, I: Integer;
begin
  FPath := Path;
  LoadFromXml;
  Result := ShowModal = mrOk;
  if Result then
  begin
    Exts := TStringList.Create;
    try
      SetLength(FlagsArr, sgExts.RowCount - 1);
      for R := 1 to sgExts.RowCount - 1 do
      begin
        Exts.Add(sgExts.Cells[0, R]);
        FlagsArr[R - 1] := CurrentFlags;
      end;
      ApplyWcxTweak(FPath, Exts, FlagsArr);
    finally
      Exts.Free;
    end;
  end;
end;

procedure TfrmTweakPacker.btnOkClick(Sender: TObject);
begin
  ModalResult := mrOk;
end;

end.
