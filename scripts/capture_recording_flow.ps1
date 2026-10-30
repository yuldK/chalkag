param(
    [Parameter(Mandatory)] [string]$Executable,
    [Parameter(Mandatory)] [string]$Output,
    [string]$HelperPath = (Join-Path $env:USERPROFILE '.codex/skills/luil-uia/scripts/uia.ps1')
)
$ErrorActionPreference = 'Stop'
. $HelperPath
$Output = [IO.Path]::GetFullPath($Output)
New-Item -ItemType Directory -Path $Output -Force | Out-Null
$previousSource = $env:CHALKAG_TEST_SYNTHETIC_CAPTURE
$env:CHALKAG_TEST_SYNTHETIC_CAPTURE = '1'
try
{
    $null = Connect-LuilApp -isolated -path $Executable -process_name ([IO.Path]::GetFileNameWithoutExtension($Executable))
    $request = @'
$script:shots = @()
$script:folder = '__OUTPUT__'
# 보조 창이 저장 중 닫히는 순간의 UIA 참조 만료는 새 표면에서 다시 읽는다.
function wait_stable
{
    param([string]$id, [string]$name, [switch]$all_surfaces, [switch]$absent, [int]$timeout_seconds = 8)
    $deadline = (Get-Date).AddSeconds($timeout_seconds)
    do
    {
        try
        {
            $element = Find-LuilElement -id $id -name $name -all_surfaces:$all_surfaces
            if ($absent -and -not $element) { return }
            if (-not $absent -and $element) { return $element }
        }
        catch
        {
            $script:luil_process.Refresh()
            if ($script:luil_process.HasExited -or (Get-Date) -ge $deadline) { throw }
        }
        Start-Sleep -Milliseconds 100
    } while ((Get-Date) -lt $deadline)
    throw "UI 상태 대기 실패: $id $name absent=$absent"
}
function checkpoint([string]$step)
{
    $step | Add-Content -LiteralPath (Join-Path $script:folder 'verification.log') -Encoding utf8
}
Set-Content -LiteralPath (Join-Path $script:folder 'verification.log') -Value '시작' -Encoding utf8
function shot([string]$step)
{
    $file = Join-Path $script:folder ('{0:D3}.png' -f $script:shots.Count)
    $null = Save-LuilShot -path $file
    $script:shots += [pscustomobject]@{ file = $file; step = $step; duration = 1400 }
}
Invoke-LuilElement (Wait-LuilElement -id open-options)
$null = Wait-LuilElement -id options-tabs
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
Invoke-LuilElement (Wait-LuilElement -id apply-region-editor -all_surfaces)
$null = Wait-LuilElement -id region-width -all_surfaces -absent
Invoke-LuilElement (Wait-LuilElement -id open-options)
$null = Wait-LuilElement -id options-tabs -absent
shot '1 · 촬영 영역 준비'
Invoke-LuilElement (Wait-LuilElement -id toggle-recording)
$null = Wait-LuilElement -name '정지'
shot '2 · 실제 녹화 작업 시작 · 합성 프레임'
Start-Sleep -Milliseconds 350
Invoke-LuilElement (Wait-LuilElement -id toggle-recording)
$null = Wait-LuilElement -id toolbar-save-recording -timeout_seconds 20
shot '3 · 정지 후 원본 보관'
if (-not (Get-LuilState (Wait-LuilElement -id take-screenshot)).enabled) { throw '보관 중 사진 비활성' }
Invoke-LuilElement (Wait-LuilElement -id take-screenshot)
$null = Wait-LuilElement -id toolbar-save-recording -timeout_seconds 20
$null = Wait-LuilElement -id saved-file
shot '4 · 보관 원본을 유지한 사진 저장'
Invoke-LuilElement (Wait-LuilElement -id toolbar-save-recording)
$null = Wait-LuilElement -name '다시 저장' -timeout_seconds 20
shot '5 · 녹화 내보내기 완료 · 결과 열기'
Invoke-LuilElement (Wait-LuilElement -id toggle-recording)
$null = Wait-LuilElement -name '정지' -timeout_seconds 20
shot '6 · 저장한 원본은 새 녹화로 바로 교체'
Start-Sleep -Milliseconds 200
Invoke-LuilElement (Wait-LuilElement -id toggle-recording)
$null = Wait-LuilElement -id toolbar-save-recording -timeout_seconds 20
shot '7 · 다음 녹화 원본 보관 · 이전 저장과 구분'
Get-LuilState (Wait-LuilElement -id take-screenshot)
Get-LuilState (Wait-LuilElement -id capture-state)
checkpoint '녹화 플로우 완료'
Invoke-LuilElement (Wait-LuilElement -name '닫기' -all_surfaces)
$null = Wait-LuilElement -id message-cancel -all_surfaces
checkpoint '보관 원본 종료 확인'
Invoke-LuilElement (Wait-LuilElement -id message-cancel -all_surfaces)
$null = Wait-LuilElement -id message-cancel -all_surfaces -absent
Invoke-LuilElement (Wait-LuilElement -id quick-edit-recording)
$null = Wait-LuilElement -id edit-play -all_surfaces -timeout_seconds 20
checkpoint '보관 원본 편집 열기'
Set-LuilValue (Wait-LuilElement -id edit-delay -all_surfaces) -value '600000'
# 입력은 잠깐 멈추면 바로 반영된다.
$null = Wait-LuilElement -name '저장 전 변경' -all_surfaces -timeout_seconds 5
Invoke-LuilElement (Wait-LuilElement -name '닫기' -all_surfaces)
$null = Wait-LuilElement -id editor-keep -all_surfaces
checkpoint '편집 종료 확인'
Invoke-LuilElement (Wait-LuilElement -id editor-keep -all_surfaces)
$null = Wait-LuilElement -id editor-keep -all_surfaces -absent
Get-LuilState (Wait-LuilElement -id edit-dirty -all_surfaces)
checkpoint '편집 종료 확인 취소'
$previousFile = (Get-LuilState (Wait-LuilElement -id saved-file)).name
Invoke-LuilElement (Wait-LuilElement -id take-screenshot)
$deadline = (Get-Date).AddSeconds(10)
do
{
    $file = Find-LuilElement -id saved-file
    if ($file -and $file.Current.Name -ne $previousFile) { break }
    Start-Sleep -Milliseconds 40
} while ((Get-Date) -lt $deadline)
if (-not $file -or $file.Current.Name -eq $previousFile) { throw '종료 확인 취소 뒤 사진 작업이 재개되지 않았습니다.' }
Invoke-LuilElement (Wait-LuilElement -name '닫기' -all_surfaces)
$null = Wait-LuilElement -id editor-keep -all_surfaces
Invoke-LuilElement (Wait-LuilElement -id editor-keep -all_surfaces)
$null = Wait-LuilElement -id editor-keep -all_surfaces -absent
'녹화·편집 종료 확인 취소 후 실제 사진 저장과 편집 변경 유지 확인'
checkpoint '종료 확인 취소 후 저장·편집 유지 검증 완료'
$script:shots | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $script:folder 'manifest.json') -Encoding utf8
'@
    $request = $request.Replace('Wait-LuilElement', 'wait_stable')
    Invoke-LuilStep ([scriptblock]::Create($request.Replace('__OUTPUT__', $Output.Replace("'", "''")))) -timeout_seconds 90
}
finally
{
    if ($script:luil_queue)
    {
        $queueRoot = [IO.Path]::GetFullPath((Join-Path ([IO.Path]::GetTempPath()) 'luil-uia'))
        if (-not [IO.Path]::GetFullPath($script:luil_queue).StartsWith($queueRoot + '\', [StringComparison]::OrdinalIgnoreCase)) { throw '격리 큐 경로 오류' }
        Disconnect-LuilApp
    }
    $env:CHALKAG_TEST_SYNTHETIC_CAPTURE = $previousSource
}
