#include <mruby.h>
#include <mruby/compile.h>

int main()
{
    mrb_state *mrb = mrb_open();
    mrb_load_string(mrb, "5.times { puts 'mruby is awesome!' }");
    mrb_close(mrb);
    return 0;
}