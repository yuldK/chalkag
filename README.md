# chalkag

작은 명령창으로 화면의 원하는 영역을 촬영하고 애니메이션 이미지로 저장하는 Windows 앱입니다.
버그 재현, 짧은 사용법 설명, 움직이는 화면 예시를 만드는 데 사용할 수 있습니다.
정지 이미지는 PNG·WebP, 애니메이션은 WebP·APNG·GIF로 저장합니다.

A Windows app for capturing a screen region as a still or animated image from a small control window.
Use it to show a bug, explain a short workflow, or share something moving on screen.
Save stills as PNG or WebP, and animations as WebP, APNG, or GIF.

[한국어 시작하기](#한국어-시작하기) · [Getting started in English](#getting-started-in-english)

![chalkag의 작은 명령창](docs/media/command-window.png)

## 한국어 시작하기

### 무엇을 할 수 있나요?

- 화면의 한 영역을 선택하고 사진이나 애니메이션 이미지로 저장합니다.
- 프레임을 드래그하거나 좌표·크기를 입력해 촬영 영역을 조절합니다. 영역 안팎의 화면을 어둡게 하거나 흐리게 하지 않습니다.
- 녹화 원본을 보관하여 형식·배율·압축 설정을 바꾸고 여러 번 저장합니다.
- 보관한 녹화 또는 GIF·WebP·APNG 파일을 열어 프레임 삭제, 표시 시간 변경, 프레임 수 줄이기, 크롭·크기 변경을 합니다.
- 단축키, 테마, 키 컬러, 영역 프리셋을 저장합니다.
- 이미지 파일을 설정창이나 편집창에 끌어 놓거나 **이미지 열기**로 선택하여 편집합니다.

동영상과 오디오는 지원하지 않습니다. 현재 인터페이스는 한국어입니다.

### 실행하기

Windows 11 x64에서 사용합니다. 준비된 `chalkag.exe`가 있다면 **쓰기 가능한 폴더**에 넣고 실행하세요.
별도 인코더나 FFmpeg 설치가 필요하지 않습니다. 소스 빌드는 아래 [빌드와 검사](#빌드와-검사--build-and-check)를 따르세요.

1. **영역 선택** 또는 `Ctrl+Shift+A`로 촬영할 부분을 드래그합니다.
2. **사진** 또는 `Ctrl+Shift+S`로 정지 이미지를 저장합니다.
3. 애니메이션은 **녹화** 또는 `Ctrl+R`으로 시작하고, 같은 버튼·단축키로 정지합니다.
4. 정지 후 **저장**을 눌러 애니메이션 파일을 만듭니다. 기본값은 자동 저장이 꺼져 있습니다.
   다음 촬영은 **새 녹화**로 시작합니다. 저장 전 원본은 교체 여부를 확인하며, 보관 중에도 사진을 찍을 수 있습니다.
5. **옵션 → 내보내기**에서 형식·출력 크기·품질·압축 속도를 조절합니다. **이미지 열기**로 기존 파일을 편집합니다.

저장 파일은 실행 파일 옆 `data/captures`, 설정은 `data/settings.json`에 있습니다.
같은 사용자 세션에서 앱을 다시 실행하면 기존 창이 활성화됩니다.

화면별 안내와 편집 방법은 [사용 설명서](docs/user-guide.md), 최신 UI의 전후 GIF와 검증 내용은 [UX 개선 기록](docs/ux-improvements.md)을 참고하세요.

## Getting started in English

### What can it do?

- Capture one screen region as a still or animated image.
- Move and resize the region by dragging its frame or entering coordinates and dimensions. The screen stays visible without dimming or blur.
- Keep the recorded source and export it again with a different format, scale, or compression setting.
- Edit a retained recording or an existing GIF, WebP, or APNG: remove frames, change durations, reduce the frame count, crop, and resize.
- Remember keyboard shortcuts, themes, accent colors, and region presets.
- Drop an image onto the settings or editor window, or choose it with **이미지 열기** (Open image).

Video and audio recording are outside the app's scope. The current interface is in Korean.

### Run the app

Use Windows 11 x64. If you have a built `chalkag.exe`, place it in a **writable folder** and run it.
No separate encoder or FFmpeg installation is required. To build from source, follow the commands below.

1. Click **영역 선택** (Select region), or press `Ctrl+Shift+A`, then drag over the area to capture.
2. Click **사진** (Screenshot), or press `Ctrl+Shift+S`, to save a still image.
3. Click **녹화** (Record), or press `Ctrl+R`, to start an animation. Use the same button or shortcut to stop.
4. Click **저장** (Save) after stopping to export the animation. Automatic saving is off by default.
   Use **새 녹화** (New recording) for the next capture. Unsaved source replacement requires confirmation; still captures remain available.
5. Use **옵션 → 내보내기** (Options → Export) to adjust format, size, quality, and compression speed. Use **이미지 열기** (Open image) to edit a file.

Images are saved in `data/captures` beside the executable; settings are stored in `data/settings.json`.
Launching another copy in the same user session activates the existing window.

See the [illustrated user guide in Korean](docs/user-guide.md) for detailed steps.

## 빌드와 검사 / Build and check

Windows 11 x64, Visual Studio 2026 또는 2022의 C++ 데스크톱 워크로드, MSVC 19.40 이상,
Windows SDK 10.0.22621.0 이상, CMake 4.2 이상, Git, PowerShell이 필요합니다. 포맷 검사에는 clang-format 22가 필요합니다.

Requires Windows 11 x64, Visual Studio 2026 or 2022 with the Desktop development with C++ workload,
MSVC 19.40+, Windows SDK 10.0.22621.0+, CMake 4.2+, Git, PowerShell, and clang-format 22 for formatting.

```powershell
./scripts/bootstrap.ps1
cmake --preset vs2026 -DCHALKAG_BUILD_TESTS=ON
cmake --build build/vs2026 --config Release
./build/vs2026/bin/Release/chalkag.exe
```

VS2022에서는 `vs2026`을 `vs2022`로 바꾸세요. / For VS2022, replace `vs2026` with `vs2022`.

```powershell
ctest --test-dir build/vs2026 -C Release --output-on-failure
cmake --build build/vs2026 --config Debug
ctest --test-dir build/vs2026 -C Debug --output-on-failure
./scripts/format.ps1
./scripts/format.ps1 -Check
./scripts/check_source_style.ps1
git diff --check
```

```powershell
cmake --install build/vs2026 --config Release --prefix bin
./bin/chalkag.exe
```

추가 옵션과 소스 구조는 [개발 안내](docs/development.md), 작성 기준은 [코드 가독성·스타일](docs/code-style.md)을 참고하세요.
See the [development guide](docs/development.md) and [code readability and style guide](docs/code-style.md) for details.

## 라이선스 / License

chalkag 자체 소스는 [MIT](LICENSE)입니다. 의존성과 아이콘에는 각 원본의 라이선스와 고지가 적용됩니다.
프로젝트 라이선스와 제삼자 고지는 실행 파일 리소스에도 포함됩니다. `data`의 개인 데이터는 소스 배포에 포함하지 않습니다.

chalkag's own source is licensed under [MIT](LICENSE). Dependencies and icons retain their respective licenses and notices.
License notices are embedded in the executable. Keep personal settings, captures, and temporary recordings in `data` out of source distributions.

[아이콘과 라이선스 / Icon assets and license](assets/README.md)
