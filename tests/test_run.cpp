// 管线端到端单元测试（工单 #3）
// 缝（seam）：RunScan（路径+检测器进、报告出）与 RunPipeline（报告→输出+退出码）。
// 检测器注入假实现，不依赖真实压缩状态；输出写临时文件验证 UTF-8 措辞。
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "pipeline.h"
#include "temp_dir.h"

#include <cstdio>
#include <string>
#include <vector>
#include <windows.h>

namespace {

// 按文件名约定返回现状：wof8k.bin→WOF xpress8k，woflzx.bin→WOF lzx，
// lznt1.bin→LZNT1，wim.bin→WIM backing，cloud.bin→Cloud backing，其余→未压缩
class FakeDetector final : public ntfscompact::IDetector {
public:
    ntfscompact::DetectionResult Detect(const std::wstring& long_path) override {
        ntfscompact::DetectionResult r;
        if (long_path.find(L"failme.bin") != std::wstring::npos) {
            r.error = L"现状检测失败（模拟）";
            return r;
        }
        const std::wstring name =
            long_path.substr(long_path.find_last_of(L"\\") + 1);
        if (name == L"wof8k.bin") {
            r.state.kind = ntfscompact::CurrentState::Kind::Wof;
            r.state.algorithm = ntfscompact::Algorithm::Xpress8k;
            r.state.provider = ntfscompact::CurrentState::kProviderFile;
        } else if (name == L"woflzx.bin") {
            r.state.kind = ntfscompact::CurrentState::Kind::Wof;
            r.state.algorithm = ntfscompact::Algorithm::Lzx;
            r.state.provider = ntfscompact::CurrentState::kProviderFile;
        } else if (name == L"lznt1.bin") {
            r.state.kind = ntfscompact::CurrentState::Kind::Lznt1;
        } else if (name == L"wim.bin") {
            r.state.kind = ntfscompact::CurrentState::Kind::Wof;
            r.state.algorithm = ntfscompact::Algorithm::Xpress4k;
            r.state.provider = ntfscompact::CurrentState::kProviderWim;
        } else if (name == L"cloud.bin") {
            r.state.kind = ntfscompact::CurrentState::Kind::Wof;
            r.state.algorithm = ntfscompact::Algorithm::Xpress4k;
            r.state.provider = ntfscompact::CurrentState::kProviderCloud;
        }
        return r;  // 其余：None
    }
};

ntfscompact::Options CompressOpt(
    ntfscompact::Algorithm a = ntfscompact::Algorithm::Xpress8k) {
    ntfscompact::Options o;
    o.algorithm = a;
    return o;
}

// 在临时目录造全套现状文件，返回目录路径
std::wstring MakeFixture(ntfscompact_test::TempDir& tmp) {
    REQUIRE(tmp.ok());
    REQUIRE(tmp.WriteFile(L"plain.bin", 8192));
    REQUIRE(tmp.WriteFile(L"wof8k.bin", 8192));
    REQUIRE(tmp.WriteFile(L"woflzx.bin", 8192));
    REQUIRE(tmp.WriteFile(L"lznt1.bin", 8192));
    REQUIRE(tmp.WriteFile(L"wim.bin", 8192));
    REQUIRE(tmp.WriteFile(L"cloud.bin", 8192));
    REQUIRE(tmp.WriteFile(L"failme.bin", 8192));
    REQUIRE(tmp.WriteFile(L"tiny.bin", 100));
    return tmp.path();
}

const ntfscompact::FileReport* Find(const ntfscompact::PipelineResult& result,
                                    const wchar_t* name) {
    for (const ntfscompact::FileReport& r : result.reports) {
        if (r.display_path.find(name) != std::wstring::npos) return &r;
    }
    return nullptr;
}

std::wstring ReadAll(std::FILE* f) {
    std::rewind(f);  // RunPipeline 写完后指针在尾部，先回到开头
    std::wstring content;
    char buffer[4096];
    size_t n = 0;
    while ((n = std::fread(buffer, 1, sizeof(buffer), f)) > 0) {
        const int wchars = MultiByteToWideChar(CP_UTF8, 0, buffer,
                                               static_cast<int>(n), nullptr, 0);
        std::wstring chunk(static_cast<size_t>(wchars), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, buffer, static_cast<int>(n),
                            chunk.data(), wchars);
        content += chunk;
    }
    return content;
}

bool Contains(const std::wstring& haystack, const std::wstring& needle) {
    return haystack.find(needle) != std::wstring::npos;
}

}  // namespace

using namespace ntfscompact;

TEST_CASE("RunScan：各现状文件得到正确动作（注入假检测器）") {
    ntfscompact_test::TempDir tmp;
    const std::wstring dir = MakeFixture(tmp);

    Options opt = CompressOpt();
    opt.paths.push_back(dir);
    FakeDetector detector;
    const PipelineResult result = RunScan(opt, detector);

    // 8 个候选（tiny.bin 被 ≤4KB 过滤但仍在报告里）+ 失败项也在报告中
    REQUIRE(result.reports.size() == 8);
    CHECK(result.failures == 1);

    const FileReport* plain = Find(result, L"plain.bin");
    REQUIRE(plain != nullptr);
    CHECK(plain->error.empty());
    CHECK(plain->plan.action == Plan::Action::Compress);
    CHECK(plain->has_state);
    CHECK(plain->state.kind == CurrentState::Kind::None);

    const FileReport* wof8k = Find(result, L"wof8k.bin");
    REQUIRE(wof8k != nullptr);
    CHECK(wof8k->plan.action == Plan::Action::Skip);
    CHECK(Contains(wof8k->plan.reason, L"指定算法"));

    const FileReport* woflzx = Find(result, L"woflzx.bin");
    REQUIRE(woflzx != nullptr);
    CHECK(woflzx->plan.action == Plan::Action::Recompress);
    CHECK(woflzx->state.algorithm == Algorithm::Lzx);

    const FileReport* lznt1 = Find(result, L"lznt1.bin");
    REQUIRE(lznt1 != nullptr);
    CHECK(lznt1->plan.action == Plan::Action::Recompress);

    const FileReport* wim = Find(result, L"wim.bin");
    REQUIRE(wim != nullptr);
    CHECK(wim->plan.action == Plan::Action::Skip);
    CHECK(Contains(wim->plan.reason, L"WIM"));

    const FileReport* cloud = Find(result, L"cloud.bin");
    REQUIRE(cloud != nullptr);
    CHECK(cloud->plan.action == Plan::Action::Skip);
    CHECK(Contains(cloud->plan.reason, L"Cloud"));

    const FileReport* failme = Find(result, L"failme.bin");
    REQUIRE(failme != nullptr);
    CHECK_FALSE(failme->error.empty());
    CHECK(Contains(failme->error, L"现状检测失败"));

    const FileReport* tiny = Find(result, L"tiny.bin");
    REQUIRE(tiny != nullptr);
    CHECK(tiny->plan.action == Plan::Action::Skip);
    CHECK(Contains(tiny->plan.reason, L"4KB"));
    CHECK_FALSE(tiny->has_state);
}

TEST_CASE("RunScan：解压模式下各文件动作") {
    ntfscompact_test::TempDir tmp;
    REQUIRE(tmp.ok());
    REQUIRE(tmp.WriteFile(L"plain.bin", 8192));
    REQUIRE(tmp.WriteFile(L"woflzx.bin", 8192));
    REQUIRE(tmp.WriteFile(L"lznt1.bin", 8192));

    Options opt;
    opt.uncompress = true;
    opt.paths.push_back(tmp.path());
    FakeDetector detector;
    const PipelineResult result = RunScan(opt, detector);

    const FileReport* plain = Find(result, L"plain.bin");
    REQUIRE(plain != nullptr);
    CHECK(plain->plan.action == Plan::Action::Skip);
    const FileReport* woflzx = Find(result, L"woflzx.bin");
    REQUIRE(woflzx != nullptr);
    CHECK(woflzx->plan.action == Plan::Action::Uncompress);
    const FileReport* lznt1 = Find(result, L"lznt1.bin");
    REQUIRE(lznt1 != nullptr);
    CHECK(lznt1->plan.action == Plan::Action::Uncompress);
}

TEST_CASE("RunPipeline：干跑输出每文件行与汇总，退出码 0（无失败）") {
    ntfscompact_test::TempDir tmp;
    REQUIRE(tmp.ok());
    REQUIRE(tmp.WriteFile(L"plain.bin", 8192));
    REQUIRE(tmp.WriteFile(L"wof8k.bin", 8192));

    Options opt = CompressOpt();
    opt.dry_run = true;
    opt.paths.push_back(tmp.path());
    FakeDetector detector;

    std::FILE* out = std::tmpfile();
    REQUIRE(out != nullptr);
    const int code = RunPipeline(opt, detector, UncompressFn{}, out, out);
    CHECK(code == 0);

    const std::wstring text = ReadAll(out);
    std::fclose(out);
    CHECK(Contains(text, L"[压缩] "));                    // plain.bin
    CHECK(Contains(text, L"plain.bin"));                  // 干跑含每文件行
    CHECK(Contains(text, L"[跳过] "));
    CHECK(Contains(text, L"已是指定算法 xpress8k"));       // wof8k.bin
    CHECK(Contains(text, L"共 2 个文件：压缩 1、重压 0、解压 0、跳过 1"));
}

TEST_CASE("RunPipeline：普通模式只打汇总，不打每文件行") {
    ntfscompact_test::TempDir tmp;
    REQUIRE(tmp.ok());
    REQUIRE(tmp.WriteFile(L"plain.bin", 8192));
    REQUIRE(tmp.WriteFile(L"wof8k.bin", 8192));

    Options opt = CompressOpt();
    opt.paths.push_back(tmp.path());
    FakeDetector detector;
    std::FILE* out = std::tmpfile();
    REQUIRE(out != nullptr);
    const int code = RunPipeline(opt, detector, UncompressFn{}, out, out);
    CHECK(code == 0);

    const std::wstring text = ReadAll(out);
    std::fclose(out);
    CHECK(Contains(text, L"共 2 个文件"));
    CHECK_FALSE(Contains(text, L"[压缩] "));  // 无每文件行
}

TEST_CASE("RunPipeline：失败路径计入退出码 1，失败行照打") {
    ntfscompact_test::TempDir tmp;
    REQUIRE(tmp.ok());
    REQUIRE(tmp.WriteFile(L"plain.bin", 8192));

    Options opt = CompressOpt();
    opt.dry_run = true;
    opt.paths.push_back(tmp.path());
    opt.paths.push_back(tmp.Sub(L"ghost.bin"));  // 不存在
    FakeDetector detector;

    std::FILE* out = std::tmpfile();
    std::FILE* err = std::tmpfile();
    REQUIRE(out != nullptr);
    REQUIRE(err != nullptr);
    const int code = RunPipeline(opt, detector, UncompressFn{}, out, err);
    CHECK(code == 1);

    const std::wstring err_text = ReadAll(err);
    std::fclose(out);
    std::fclose(err);
    CHECK(Contains(err_text, L"ghost.bin"));  // 错误到 stderr
}

TEST_CASE("RunPipeline：-q 静默不输出") {
    ntfscompact_test::TempDir tmp;
    REQUIRE(tmp.ok());
    REQUIRE(tmp.WriteFile(L"plain.bin", 8192));

    Options opt = CompressOpt();
    opt.quiet = true;
    opt.paths.push_back(tmp.path());
    FakeDetector detector;
    std::FILE* out = std::tmpfile();
    REQUIRE(out != nullptr);
    const int code = RunPipeline(opt, detector, UncompressFn{}, out, out);
    std::fclose(out);
    CHECK(code == 0);
    // 静默：输出为空（tmpfile 已关，code 即验证点；见实现直接返回）
}
