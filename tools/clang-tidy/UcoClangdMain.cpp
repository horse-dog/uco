namespace clang::clangd {
int clangdMain(int argc, char **argv);
}

int main(int argc, char **argv)
{
    return clang::clangd::clangdMain(argc, argv);
}
