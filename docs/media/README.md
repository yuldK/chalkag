# 사용 설명서 이미지

실제 앱 및 테스트 프리뷰를 UI Automation으로 확인한 화면과 사용자가 제공한 애니메이션 예제다. 문서에서는 상대 경로로 참조한다.

| 파일 | 내용 |
| --- | --- |
| `command-window.png` | 명령창 |
| `capture-options.png` | 캡쳐 형식·FPS·출력 크기 |
| `region-settings.png` | 영역 좌표·크기 설정 |
| `output-options.png` | 원본 보관과 저장 설정 |
| `animation-editor.png` | 자체 예제 GIF를 연 최신 편집창 |
| `image-open.png` | 파일 드롭 안내와 이미지 열기 버튼 |
| `select-region.webp` | 촬영 영역의 크기 조절 |
| `record-and-save.webp` | 촬영 영역과 움직이는 예제 화면 |
| `edit-animation.webp` | 크롭 영역 선택과 결과 미리보기 |
| `ux-comparison.gif` | 같은 단계의 기존·최신 UI 비교 |
| `ux-before.gif` / `ux-after.gif` | 기존·최신 UI 흐름 |
| `ux-editor-flow.gif` | 예제 편집·입력 보호·종료 확인·저장 |
| `ux-recording-flow.gif` | 합성 프레임을 입력한 실제 앱의 녹화·사진·저장·다음 녹화 |
| `ux-comparison.png` | 단축키 초안 유지 비교 정지 화면 |
| `ux-presets.png` | 이름 변경·목록 선택·삭제 취소를 확인한 프리셋 |

화면을 갱신할 때는 UI Automation으로 조작하고 실제 상태를 확인한다.
촬영 대상의 초점을 보조 창으로 옮기면 키보드 초점 테두리 없이 찍을 수 있다.
앱은 자기 창을 캡쳐에서 제외하므로 UI 설명 화면에는 별도 캡쳐 도구를 사용한다.
공개 이미지에는 계정·개인 경로·개인 캡쳐가 없는지 확인한다.

전후 비교 GIF의 마지막 저장 완료 장면은 테스트 상태 프리뷰다. 별도 촬영 흐름은 합성 프레임을 실제 앱 런타임으로 처리했다.
촬영·재현 방법과 검증 범위는 [UX 개선 기록](../ux-improvements.md)에 적었다.
