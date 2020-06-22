#include <mruby.h>
#include <mruby/compile.h>

int main()
{
    FILE *fp;
    if ((fp = fopen("./hello.rb", "r")) == NULL) {
        return 1;
    }

    mrb_state *mrb = mrb_open();
    mrb_load_file(mrb, fp);
    mrb_close(mrb);
    return 0;
}