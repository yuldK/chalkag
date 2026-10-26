param(
    [Parameter(Mandatory)] [string]$Executable,
    [Parameter(Mandatory)] [string]$Output,
    [ValidateSet('before', 'after')] [string]$Variant = 'after',
    [string]$HelperPath = (Join-Path $env:USERPROFILE '.codex/skills/luil-uia/scripts/uia.ps1')
)
$ErrorActionPreference = 'Stop'
. $HelperPath
$Output = [IO.Path]::GetFullPath($Output)
$Executable = [IO.Path]::GetFullPath($Executable)
New-Item -ItemType Directory -Path $Output -Force | Out-Null
$null = Connect-LuilApp -isolated -path $Executable -process_name ([IO.Path]::GetFileNameWithoutExtension($Executable))
try
{
    $scriptText = @'
$script:shots = @()
$script:shotIndex = 0
$script:shotFolder = '__OUTPUT__'
$script:variant = '__VARIANT__'
function shot([string]$step, [string]$target = '')
{
    $surface = $null
    if ($target)
    {
        $surface = Wait-LuilElement -id $target -all_surfaces
        $walker = [System.Windows.Automation.TreeWalker]::ControlViewWalker
        while ($surface.Current.NativeWindowHandle -eq 0) { $surface = $walker.GetParent($surface) }
    }
    for ($frame = 0; $frame -lt 3; ++$frame)
    {
        $file = Join-Path $script:shotFolder ('{0:D3}.png' -f $script:shotIndex++)
        $null = Save-LuilShot -path $file -surface $surface
        $script:shots += [pscustomobject]@{ file = $file; step = $step; duration = $(if ($frame -eq 2) { 1100 } else { 120 }) }
        Start-Sleep -Milliseconds 120
    }
}
shot '시작 · 다음 행동 찾기'
Invoke-LuilElement (Wait-LuilElement -id open-options)
$null = Wait-LuilElement -id options-tabs
shot '캡쳐와 출력 설정'
Select-LuilItem (Wait-LuilElement -id region)
$null = Wait-LuilElement -id edit-region
Invoke-LuilElement (Wait-LuilElement -id edit-region)
$null = Wait-LuilElement -id region-width -all_surfaces
Add-Type -AssemblyName System.Windows.Forms
$screen = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
Set-LuilValue (Wait-LuilElement -id region-x -all_surfaces) -value ($screen.Right - 340).ToString()
Set-LuilValue (Wait-LuilElement -id region-y -all_surfaces) -value ($screen.Bottom - 240).ToString()
Set-LuilValue (Wait-LuilElement -id region-width -all_surfaces) -value '320'
Set-LuilValue (Wait-LuilElement -id region-height -all_surfaces) -value '200'
shot '영역 위치와 크기 입력' 'region-width'
Invoke-LuilElement (Wait-LuilElement -id apply-region-editor -all_surfaces)
$null = Wait-LuilElement -id region-width -all_surfaces -absent
shot '촬영 영역 준비'
Select-LuilItem (Wait-LuilElement -id preferences)
$null = Wait-LuilElement -id shortcut-recording
Set-LuilValue (Wait-LuilElement -id shortcut-recording) -value 'Ctrl+Alt+F9'
shot '단축키 수정 · 적용 전'
Select-LuilItem (Wait-LuilElement -id capture)
$null = Wait-LuilElement -id fps
Select-LuilItem (Wait-LuilElement -id preferences)
$null = Wait-LuilElement -id shortcut-recording
Get-LuilState (Wait-LuilElement -id shortcut-recording)
shot '탭을 왕복한 뒤 입력 상태'
Select-LuilItem (Wait-LuilElement -id output)
$null = Wait-LuilElement -id open-animation
shot '형식·품질·압축 설정'
$open = Wait-LuilElement -id open-animation
if (@($open.GetSupportedPatterns() | ForEach-Object ProgrammaticName) -contains 'ScrollItemPatternIdentifiers.Pattern')
{
    $open.GetCurrentPattern([System.Windows.Automation.ScrollItemPattern]::Pattern).ScrollIntoView()
}
Invoke-LuilElement (Wait-LuilElement -id open-animation)
$null = Wait-LuilElement -id edit-browse -all_surfaces
shot '이미지 편집 시작' 'edit-browse'
$script:shots | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $script:shotFolder 'manifest.json') -Encoding utf8
'@
    $scriptText = $scriptText.Replace('__OUTPUT__', $Output.Replace("'", "''")).Replace('__VARIANT__', $Variant)
    Invoke-LuilStep ([scriptblock]::Create($scriptText)) -timeout_seconds 55
}
finally
{
    $queue = [IO.Path]::GetFullPath($script:luil_queue)
    $queueRoot = [IO.Path]::GetFullPath((Join-Path ([IO.Path]::GetTempPath()) 'luil-uia'))
    if (-not $queue.StartsWith($queueRoot + '\', [StringComparison]::OrdinalIgnoreCase)) { throw '격리 큐 경로를 확인할 수 없습니다.' }
    Disconnect-LuilApp
}
