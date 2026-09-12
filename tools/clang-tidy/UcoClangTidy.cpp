#include "clang-tidy/ClangTidy.h"
#include "clang-tidy/ClangTidyCheck.h"
#include "clang-tidy/ClangTidyModule.h"
#include "clang-tidy/ClangTidyModuleRegistry.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/Stmt.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "llvm/ADT/StringRef.h"

namespace clang::tidy::uco {
namespace {

class DiscardedCoawaitLockCheck : public ClangTidyCheck {
public:
  DiscardedCoawaitLockCheck(llvm::StringRef Name, ClangTidyContext *Context)
      : ClangTidyCheck(Name, Context) {}

  void registerMatchers(ast_matchers::MatchFinder *Finder) override {
    using namespace ast_matchers;
    Finder->addMatcher(
        coawaitExpr(hasDescendant(callExpr(callee(functionDecl(matchesName(
                        "(^|::)uco::(ulock_guard|unique_ulock)$"))))))
            .bind("lock-await"),
        this);
  }

  void check(const ast_matchers::MatchFinder::MatchResult &Result) override {
    const auto *Await = Result.Nodes.getNodeAs<CoawaitExpr>("lock-await");
    if (Await == nullptr || Result.Context == nullptr)
      return;

    DynTypedNode Node = DynTypedNode::create(*Await);
    while (true) {
      const auto Parents = Result.Context->getParents(Node);
      if (Parents.size() != 1)
        return;

      const DynTypedNode &Parent = Parents[0];
      if (Parent.get<ExprWithCleanups>() != nullptr ||
          Parent.get<MaterializeTemporaryExpr>() != nullptr ||
          Parent.get<CXXBindTemporaryExpr>() != nullptr ||
          Parent.get<ImplicitCastExpr>() != nullptr ||
          Parent.get<ParenExpr>() != nullptr) {
        Node = Parent;
        continue;
      }

      if (Parent.get<CompoundStmt>() != nullptr) {
        diag(Await->getBeginLoc(),
             "the lock object returned by co_await is discarded and will be "
             "released at the end of this statement; store it in a local "
             "variable");
      }
      return;
    }
  }
};

class UcoModule : public ClangTidyModule {
public:
  void addCheckFactories(ClangTidyCheckFactories &Factories) override {
    Factories.registerCheck<DiscardedCoawaitLockCheck>(
        "uco-discarded-coawait-lock");
  }
};

static ClangTidyModuleRegistry::Add<UcoModule>
    Module("uco-module", "UCO project clang-tidy checks");

} // namespace

volatile int UcoModuleAnchorSource = 0;

} // namespace clang::tidy::uco
