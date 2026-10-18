# chalkag 앱 아이콘

`device-camera` Codicon을 짙은 청록색 사각 배경과 민트색으로 렌더링했다.
`chalkag.ico`에는 16·20·24·32·40·48·64·128·256픽셀 이미지가 포함되며 `chalkag.png`는 256픽셀 미리보기다.

원본: Microsoft [vscode-codicons](https://github.com/microsoft/vscode-codicons),
고정 버전 `0.0.46-24`, `third_party/luil/assets/codicons/codicon.ttf`의 `device-camera` 글리프.
색상·배경·배치·크기 조정은 chalkag에서 적용했다.
원본 아이콘 라이선스는 [CC BY 4.0](../third_party/luil/assets/codicons/LICENSE)이며
Codicons 고지는 기존 luil 라이선스·제3자 고지와 함께 실행 파일에 포함된다.

아이콘은 저장소에 포함되므로 앱 빌드·실행에 Python이 필요하지 않다.
재생성에는 Python과 Pillow를 사용한다.

```powershell
python scripts/create_app_icon.py
```
