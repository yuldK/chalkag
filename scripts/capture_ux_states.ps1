param(
    [Parameter(Mandatory)] [string]$Executable,
    [Parameter(Mandatory)] [string]$Output,
    [ValidateSet('saved', 'editor')] [string]$Scenario = 'saved',
    [string]$Sample,
    [string]$HelperPath = (Join-Path $env:USERPROFILE '.codex/skills/luil-uia/scripts/uia.ps1')
)
$ErrorActionPreference = 'Stop'
. $HelperPath
$Output = [IO.Path]::GetFullPath($Output)
New-Item -ItemType Directory -Path $Output -Force | Out-Null
$savedEnvironment = $env:CHALKAG_TEST_SAVED_FILE_PREVIEW
$editorEnvironment = $env:CHALKAG_TEST_EDITOR_PREVIEW
if ($Scenario -eq 'saved') { $env:CHALKAG_TEST_SAVED_FILE_PREVIEW = '1'; $env:CHALKAG_TEST_EDITOR_PREVIEW = $null }
else { $env:CHALKAG_TEST_EDITOR_PREVIEW = [IO.Path]::GetFullPath($Sample); $env:CHALKAG_TEST_SAVED_FILE_PREVIEW = $null }
try
{
    $null = Connect-LuilApp -isolated -path $Executable -process_name ([IO.Path]::GetFileNameWithoutExtension($Executable))
    $request = @'
$script:shots = @()
$script:folder = '__OUTPUT__'
function shot([string]$step, [string]$target = '')
{
    $surface = $null
    if ($target)
    {
        $surface = Wait-LuilElement -id $target -all_surfaces
        $walker = [System.Windows.Automation.TreeWalker]::ControlViewWalker
        while ($surface.Current.NativeWindowHandle -eq 0) { $surface = $walker.GetParent($surface) }
    }
    $file = Join-Path $script:folder ('{0:D3}.png' -f $script:shots.Count)
    $null = Save-LuilShot -path $file -surface $surface
    $script:shots += [pscustomobject]@{ file = $file; step = $step; duration = 1700 }
}
if ('__SCENARIO__' -eq 'saved')
{
    $null = Wait-LuilElement -id saved-file
    shot '저장 완료 상태 · 동일 테스트 입력'
    Get-LuilState (Wait-LuilElement -id saved-file)
}
else
{
    $null = Wait-LuilElement -id edit-play -all_surfaces -timeout_seconds 20
    shot '예제 애니메이션 편집' 'edit-play'
    Set-LuilValue (Wait-LuilElement -id edit-delay -all_surfaces) -value '0'
    $null = Wait-LuilElement -id edit-reset-inputs -all_surfaces -timeout_seconds 5
    shot '받아들일 수 없는 입력을 표시' 'edit-play'
    Set-LuilValue (Wait-LuilElement -id edit-delay -all_surfaces) -value '123'
    $null = Wait-LuilElement -id edit-reset-inputs -all_surfaces -absent -timeout_seconds 5
    shot '입력은 바로 반영하고 저장 전 상태로 표시' 'edit-play'
    Invoke-LuilElement (Wait-LuilElement -name '닫기' -all_surfaces)
    $null = Wait-LuilElement -id editor-keep -all_surfaces
    shot '종료 전에 편집 내용 보호' 'editor-keep'
    Invoke-LuilElement (Wait-LuilElement -id editor-keep -all_surfaces)
    $null = Wait-LuilElement -id editor-keep -all_surfaces -absent
    $quality = Wait-LuilElement -id edit-webp-quality -all_surfaces
    (Wait-LuilElement -id edit-compression -all_surfaces).GetCurrentPattern([System.Windows.Automation.ScrollItemPattern]::Pattern).ScrollIntoView()
    Get-LuilState (Wait-LuilElement -id edit-webp-quality -all_surfaces)
    shot '편집창에서 품질과 압축 속도 설정' 'edit-play'
    Invoke-LuilElement (Wait-LuilElement -id edit-export -all_surfaces)
    $null = Wait-LuilElement -id edit-open-result -all_surfaces -timeout_seconds 20
    Get-LuilState (Wait-LuilElement -id edit-dirty -all_surfaces)
    shot '편집본 저장 완료 · 파일 열기' 'edit-play'
}
$script:shots | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $script:folder 'manifest.json') -Encoding utf8
'@
    $request = $request.Replace('__OUTPUT__', $Output.Replace("'", "''")).Replace('__SCENARIO__', $Scenario)
    Invoke-LuilStep ([scriptblock]::Create($request)) -timeout_seconds 45
}
finally
{
    if ($script:luil_queue)
    {
        $queueRoot = [IO.Path]::GetFullPath((Join-Path ([IO.Path]::GetTempPath()) 'luil-uia'))
        if (-not [IO.Path]::GetFullPath($script:luil_queue).StartsWith($queueRoot + '\', [StringComparison]::OrdinalIgnoreCase)) { throw '격리 큐 경로 오류' }
        Disconnect-LuilApp
    }
    $env:CHALKAG_TEST_SAVED_FILE_PREVIEW = $savedEnvironment
    $env:CHALKAG_TEST_EDITOR_PREVIEW = $editorEnvironment
}
