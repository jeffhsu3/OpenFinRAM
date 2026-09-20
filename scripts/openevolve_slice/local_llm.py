"""OpenEvolve client for a small reasoning model behind a local OpenAI-style server.

Two things the stock client gets wrong for a model like Nanbeige 3B on vLLM:

* It cannot turn the model's reasoning off.  With it on, the model spends
  over a thousand tokens deliberating about a one-line function, inline in
  ``content``; with ``chat_template_kwargs={"enable_thinking": false}`` the same
  request is 18 tokens.  The flag goes in ``extra_body``.
* OpenEvolve's full-rewrite parser takes the *first* fenced block in the
  reply.  A reasoning model drafts code while thinking and gives its answer
  last, so with reasoning on this keeps only the last fenced block.

OpenEvolve serialises model configs into its worker processes and calls
``init_client(model_cfg)`` there, so `make_local_llm` has to be a module-level
function in an importable module; run_openevolve_slice.py puts this directory
on PYTHONPATH.  Settings travel in the environment for the same reason.
"""

from __future__ import annotations

import os
import re
from typing import Any

from openevolve.llm.openai import OpenAILLM

THINKING_ENV = "SLICE_EVOLVE_THINKING"
_FENCE = re.compile(r"```(?:python|py)?\n(.*?)```", re.DOTALL)


_OPEN_FENCE = re.compile(r"```(?:python|py)?\n")


def last_code_block(text: str, *, strict: bool = False) -> str:
    """Reduce a reply to its final fenced block.

    A reply that ran out of tokens may end inside its last block; that block
    is still the answer, so it is taken up to the end of the text.  With no
    fence at all the text is returned as is, or, when `strict`, nothing: tens
    of kilobytes of deliberation are not a program, and OpenEvolve would
    otherwise try to evaluate them as one.
    """
    text = text or ""
    blocks = _FENCE.findall(text)
    opened = list(_OPEN_FENCE.finditer(text))
    if opened and (not blocks or text.count("```") % 2):
        blocks.append(text[opened[-1].end():])
    if blocks:
        return f"```python\n{blocks[-1].strip()}\n```"
    return "" if strict else text


class LocalChatLLM(OpenAILLM):
    """`OpenAILLM` plus the chat-template switch and answer extraction."""

    def __init__(self, model_cfg: Any) -> None:
        super().__init__(model_cfg)
        self.enable_thinking = os.environ.get(THINKING_ENV, "0") == "1"

    async def _call_api(self, params: dict[str, Any]) -> str:
        params = dict(params)
        extra = dict(params.get("extra_body") or {})
        extra.setdefault("chat_template_kwargs", {"enable_thinking": self.enable_thinking})
        params["extra_body"] = extra
        text = await super()._call_api(params)
        return last_code_block(text, strict=True) if self.enable_thinking else (text or "")


def make_local_llm(model_cfg: Any) -> LocalChatLLM:
    return LocalChatLLM(model_cfg)
