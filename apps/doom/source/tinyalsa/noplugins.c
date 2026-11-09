// SPDX-License-Identifier: MIT
/*
 * tinyalsa 2.0.0 without plugins (no dlopen on the badge): pcm_close() still
 * calls this outside its TINYALSA_USES_PLUGINS guards. Without plugins there
 * is never a node to close.
 */
struct snd_node;

void snd_utils_close_dev_node(struct snd_node *node)
{
	(void)node;
}
