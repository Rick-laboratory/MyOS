CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -Isdk/include
LDFLAGS = -lpthread -lm

# Build every C file in this folder so new app modules like app_calc.c
# cannot silently be left out again.
ALL_SRCS := $(sort $(wildcard *.c))
SRCS := $(filter-out myos_apphost_child.c,$(ALL_SRCS))
OBJS := $(SRCS:.c=.o)
TARGET = myos_input
CHILD_TARGET = myos_apphost_child

.PHONY: all clean print-sources
all: print-sources $(TARGET) $(CHILD_TARGET)

print-sources:
	@echo "BUILD: myos_v238_menu_overlay_damage_signature"
	@echo "SRCS: $(SRCS)"
	@echo "CHILD: myos_apphost_child.c"
	@echo "NOTE: smoke.c, app_calc.c and app_editor.c MUST be listed above"

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(OBJS) -o $(TARGET) $(LDFLAGS)
	@echo "gebaut: ./$(TARGET)  [BUILD: myos_v238_menu_overlay_damage_signature]"

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

$(CHILD_TARGET): myos_apphost_child.c
	$(CC) $(CFLAGS) $< -o $(CHILD_TARGET)
	@echo "gebaut: ./$(CHILD_TARGET)  [BUILD: myos_v238_menu_overlay_damage_signature]"

clean:
	rm -f $(TARGET) $(CHILD_TARGET) $(OBJS) myos_apphost_child.o
