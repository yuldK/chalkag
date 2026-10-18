# CGIF 정적 GIF 인코더

[dloebl/cgif](https://github.com/dloebl/cgif)의 커밋
`b68f18ac125085923cae83b458e019e89feebe25`를 고정했다. MIT 라이선스 원문과
저작권 고지는 `LICENSE`에 보관한다. 다운로드 원본과 작업 트리 CRLF 파일의 SHA-256은 `source.json`에 있다.
줄 끝을 UTF-8 무 BOM·CRLF로 정규화한 것 외에는 원본 소스를 수정하지 않았다.

`inc/cgif.h`, `inc/cgif_raw.h`, `src/cgif.c`, `src/cgif_raw.c`만 사용한다. 공개 색 인덱스 API로
호출하므로 자체 RGB 양자화 파일이나 다른 패키지가 필요 없다. CMake에서 C99 `chalkag_cgif`
정적 라이브러리로 기본 빌드하고 앱의 GIF 저장에 연결한다. 별도 외부 런타임을 설치하지 않는다.

MIT 저작권·허가·면책 고지를 소스에 유지하고 실행 파일의 제삼자 고지 리소스에 포함한다.
이전 LGPL 파생 LZW와 FFmpeg 참고 소스는 제거했다. Skia·luil과 그 의존성은 각각의 고지를 유지한다.
