# 코드 가독성·스타일

한 함수 안에서도 **입력 확인 → 준비 → 실제 처리 → 결과 반영**처럼 의미가 바뀌는 지점에 공백 줄을 둔다.
같은 상태를 구성하는 연속 할당이나 같은 계산을 위한 선언은 한 묶음으로 유지한다. 문장마다 빈 줄을 넣는 방식은 피한다.

## 호출과 변수

짧고 바로 이해할 수 있는 `publish()`, `std::move(value)`, 단순 접근자 호출은 그대로 쓴다.
긴 중첩 계산, 옵션 선택, 버퍼·배치 구성은 먼저 이름 있는 변수로 만든 뒤 호출한다.
변수는 계산의 의미를 드러내야 하며 평가 순서, 단락 평가, 참조·소유권 수명을 바꾸지 않는다.

```cpp
// 목표 FPS와 달리 프레임 표시는 실제 수집 시각을 기준으로 한다.
const double timestamp { std::chrono::duration<double>(captured - start).count() };
const auto pixels { buffer.pixels() };

if (!recorder.append(pixels, timestamp))
{
    return false;
}
```

서로 독립된 명령이나 반복되는 절차는 역할을 설명하는 함수로 분리한다.
단순한 한 문장에 이름만 붙인 함수는 필요할 때만 만든다. 상태 변경은 logic driver에 두고 UI tree는 상태에서 조립한다.

## 주석과 배치

소스 주석은 한국어로 작성한다. 의미 단위의 시작에 처리 목적, 순서의 이유, 보존할 조건을 설명한다.
할당·반복을 그대로 번역하는 주석보다 원본을 보존하는 이유나 시간 반올림 정책을 설명하는 주석이 유용하다.

C++는 Allman 중괄호, 공백 4칸, `snake_case`, 중괄호 초기화를 사용한다.
namespace의 여는 중괄호만 같은 줄에 둔다. 조건·반복문의 본문에는 중괄호를 사용한다.
함수 사이와 switch의 서로 다른 동작 사이도 공백 줄로 구분한다.

## 자동 포맷과 검사

텍스트 파일은 UTF-8 무 BOM·CRLF다. `.clang-format`은 한 줄 120자를 기준으로 긴 인수와 매개변수를 나누고 함수 사이를 분리한다.
`scripts/format.ps1`은 clang-format 22 출력에서 여러 줄 중괄호의 닫힘을 독립된 줄로 정리한다.
`-Check`도 같은 정규화 결과와 현재 파일을 비교하여 쓰기 모드와 검사 모드에 동일한 기준을 사용한다.

```powershell
./scripts/format.ps1
./scripts/format.ps1 -Check
./scripts/check_source_style.ps1
git diff --check
```

자동 포맷만으로 의미 단위가 정해지지는 않는다. 공백·주석·변수가 읽는 사람에게 처리 흐름을 설명하는지도 검토한다.
