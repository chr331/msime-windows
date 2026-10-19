"""Compile the actual voice ownership and legacy reset/cancel bodies against SDK mocks."""
from pathlib import Path
import sys
from generate_reconnect_fixture import extract_body


def generate(root, output):
    root, output = Path(root), Path(output)
    voice = (root / 'IME/MetasequoiaIME_VoiceDelivery.cpp').read_text(encoding='utf-8')
    state = 'struct VoiceDeliveryUiState ' + extract_body(voice, 'struct VoiceDeliveryUiState') + ';\n'
    (output / 'voice_reset_state.h').write_text(state, encoding='utf-8')
    functions = [
        (voice, 'bool CMetasequoiaIME::_ShouldPreserveVoiceComposition(ITfComposition *composition) const'),
        ((root / 'IME/MetasequoiaIME.cpp').read_text(encoding='utf-8'),
         'void CMetasequoiaIME::_RequestLocalSessionReset(_In_opt_ ITfContext *preferredContext, UINT resetToken)'),
        ((root / 'Key/KeyHandler.cpp').read_text(encoding='utf-8'),
         'HRESULT CMetasequoiaIME::_HandleCancel(TfEditCookie ec, _In_ ITfContext *pContext)'),
        ((root / 'Composition/Composition.cpp').read_text(encoding='utf-8'),
         'HRESULT CMetasequoiaIME::_RemoveDummyCompositionForComposing(TfEditCookie ec, _In_ ITfComposition *pComposition)'),
        ((root / 'Composition/EndComposition.cpp').read_text(encoding='utf-8'),
         'void CMetasequoiaIME::_TerminateComposition(TfEditCookie ec, _In_ ITfContext *pContext, BOOL isCalledFromDeactivate,\n                                            _Out_opt_ HRESULT *result)'),
    ]
    (output / 'voice_reset_source.h').write_text('\n'.join(
        signature + ' ' + extract_body(source, signature) for source, signature in functions), encoding='utf-8')


if __name__ == '__main__':
    generate(*sys.argv[1:])
