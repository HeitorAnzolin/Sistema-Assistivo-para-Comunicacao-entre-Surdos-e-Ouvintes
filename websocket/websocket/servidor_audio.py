import array
import asyncio
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
import unicodedata
import wave

import edge_tts
import websockets

HOST = "0.0.0.0"
PORT = 8765
VOICE = "pt-BR-FranciscaNeural"
IDIOMA = "pt-BR"
SAMPLE_RATE = 16000
CHUNK = 2048
PASTA_GRAVACOES = "gravacoes"

# Caminho automatico para Oficinas 1/modeloLibras/reconhecer.py
CAMINHO_RECONHECER = Path(__file__).resolve().parent.parent / "modeloLibras" / "reconhecer.py"
proc_libras = None

clientes = set()      # ESP32 conectados
libras = set()        # reconhecer.py conectado
eco = False
ultima_wav = None
ultima_transcricao = ""
modo_atual = "outro"  # Comeca fechado; abre quando o usuario tocar em "Libras" no Arduino
tarefas = set()


def gerir_janela_libras(abrir: bool):
    """Abre ou fecha o reconhecer.py automaticamente conforme a tela do Arduino."""
    global proc_libras
    esta_aberto = proc_libras is not None and proc_libras.poll() is None

    if abrir and not esta_aberto:
        if CAMINHO_RECONHECER.exists():
            print(f"[AUTO] Abrindo camera de Libras ({CAMINHO_RECONHECER.name})...")
            proc_libras = subprocess.Popen(
                [sys.executable, str(CAMINHO_RECONHECER)],
                cwd=str(CAMINHO_RECONHECER.parent)
            )
        else:
            print(f"[ERRO] Arquivo nao encontrado em: {CAMINHO_RECONHECER}")
    elif not abrir and esta_aberto:
        print("[AUTO] Fechando camera de Libras...")
        proc_libras.terminate()
        proc_libras = None


def disparar(coro):
    t = asyncio.create_task(coro)
    tarefas.add(t)
    t.add_done_callback(tarefas.discard)


def sem_acentos(texto: str, limite: int = 200) -> str:
    t = unicodedata.normalize("NFKD", texto)
    t = "".join(c for c in t if not unicodedata.combining(c))
    t = "".join(c if 32 <= ord(c) < 127 else " " for c in t)
    return " ".join(t.split())[:limite]


async def enviar_esp32(msg: str):
    for ws in list(clientes):
        try:
            await ws.send(msg)
        except Exception:
            pass


async def enviar_libras(msg: str):
    for ws in list(libras):
        try:
            await ws.send(msg)
        except Exception:
            pass


# ------------------------------------------------------------ PC -> ESP32 (TTS)
async def gerar_pcm(texto: str) -> bytes:
    with tempfile.TemporaryDirectory() as pasta:
        mp3 = os.path.join(pasta, "tts.mp3")
        await edge_tts.Communicate(texto, VOICE).save(mp3)

        if shutil.which("ffmpeg"):
            proc = await asyncio.create_subprocess_exec(
                "ffmpeg", "-loglevel", "error", "-i", mp3,
                "-ar", str(SAMPLE_RATE), "-ac", "1", "-f", "s16le", "-",
                stdout=asyncio.subprocess.PIPE,
            )
            pcm, _ = await proc.communicate()
            return pcm

        import miniaudio
        som = miniaudio.decode_file(
            mp3,
            output_format=miniaudio.SampleFormat.SIGNED16,
            nchannels=1,
            sample_rate=SAMPLE_RATE,
        )
        return som.samples.tobytes()


async def enviar_audio(pcm: bytes):
    for ws in list(clientes):
        try:
            await ws.send(f"tts_start:{SAMPLE_RATE}")
            for i in range(0, len(pcm), CHUNK):
                await ws.send(pcm[i:i + CHUNK])
            await ws.send("tts_end")
        except Exception:
            pass


async def falar(texto: str):
    texto = texto.strip()
    if not texto:
        return
    if not clientes:
        print("Nenhum ESP32 conectado.")
        return
    await enviar_esp32("falando")
    try:
        t0 = time.time()
        pcm = await gerar_pcm(texto)
        print(f"TTS pronto: {len(pcm)} bytes em {time.time() - t0:.1f} s. Enviando...")
        await enviar_audio(pcm)
    except Exception as e:
        print(f"Erro ao gerar ou enviar a voz: {e!r}")
        await enviar_esp32("erro:Falha ao gerar a voz")


# ------------------------------------------------------------ ESP32 -> PC (Microfone)
def salvar_wav(pcm: bytes, taxa: int) -> str:
    os.makedirs(PASTA_GRAVACOES, exist_ok=True)
    caminho = os.path.join(PASTA_GRAVACOES, time.strftime("gravacao_%Y%m%d_%H%M%S.wav"))
    with wave.open(caminho, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(taxa)
        w.writeframes(pcm)
    return caminho


def nivel_pico(pcm: bytes) -> int:
    amostras = array.array("h")
    amostras.frombytes(pcm[: len(pcm) // 2 * 2])
    if sys.byteorder == "big":
        amostras.byteswap()
    return max((abs(a) for a in amostras), default=0)


def transcrever(caminho: str) -> str | None:
    try:
        import speech_recognition as sr
    except ImportError:
        print("(SpeechRecognition nao instalado: pip install SpeechRecognition)")
        return None

    r = sr.Recognizer()
    with sr.AudioFile(caminho) as fonte:
        audio = r.record(fonte)
    try:
        return r.recognize_google(audio, language=IDIOMA)
    except sr.UnknownValueError:
        return ""
    except sr.RequestError as e:
        print(f"Erro no servico Google STT: {e}")
        return None


async def finalizar_gravacao(pcm: bytes, taxa: int):
    global ultima_wav
    if not pcm:
        print("[MIC] nenhum dado recebido.")
        return
    caminho = salvar_wav(pcm, taxa)
    ultima_wav = caminho
    dur = len(pcm) / (taxa * 2)
    pico = nivel_pico(pcm)
    print(f"[MIC] salvo em {caminho} ({dur:.1f} s, pico {pico}/32767 = {pico * 100 // 32767}%)")


async def transcrever_ultima(responder_esp32: bool = True):
    global ultima_transcricao
    try:
        if not ultima_wav or not os.path.exists(ultima_wav):
            print("[STT] nenhuma gravacao para transcrever.")
            if responder_esp32:
                await enviar_esp32("erro:Nenhuma gravacao encontrada")
            return
        loop = asyncio.get_running_loop()
        texto = await loop.run_in_executor(None, transcrever, ultima_wav)
        if texto is None:
            if responder_esp32:
                await enviar_esp32("erro:Falha no servico de voz")
            return
        if texto == "":
            print("[STT] nao entendi nada na gravacao.")
            if responder_esp32:
                await enviar_esp32("erro:Nao entendi o audio")
            return
        ultima_transcricao = texto
        print(f"[STT] transcricao: {texto}")
        if responder_esp32:
            await enviar_esp32("texto:" + sem_acentos(texto, 118))
        if eco:
            await falar(texto)
    except Exception as e:
        print(f"[STT] erro inesperado: {e!r}")
        if responder_esp32:
            await enviar_esp32("erro:Erro ao transcrever")


# ------------------------------------------------------------ Conexoes WebSocket
async def tratar_libras(msg: str):
    if msg.startswith("frase:"):
        await enviar_esp32("palavra:" + sem_acentos(msg[6:], 200))
    elif msg.startswith("falar:"):
        frase = msg[6:].strip().lower()
        if frase:
            print(f"[LIBRAS] falar: {frase}")
            disparar(falar(frase))
    else:
        print(f"[LIBRAS] {msg}")


async def tratador(ws):
    global modo_atual
    papel = None
    gravacao = None
    taxa = SAMPLE_RATE
    print(f"[+] conexao: {ws.remote_address}")
    try:
        async for msg in ws:
            if isinstance(msg, bytes):
                if gravacao is not None:
                    gravacao.extend(msg)
                continue

            if msg.startswith("ready:"):
                papel = "esp32"
                clientes.add(ws)
                print(f"[ESP32] conectado: {msg[6:]}")
            elif msg == "hello:libras":
                papel = "libras"
                libras.add(ws)
                print("[LIBRAS] reconhecedor conectado")
                await ws.send("modo:" + modo_atual)
            elif papel == "libras":
                await tratar_libras(msg)
            elif msg.startswith("mic_start:"):
                taxa = int(msg.split(":", 1)[1])
                gravacao = bytearray()
                print("[MIC] recebendo audio...")
            elif msg.startswith("mic_end"):
                pcm = bytes(gravacao or b"")
                gravacao = None
                disparar(finalizar_gravacao(pcm, taxa))
            elif msg.startswith("pong:"):
                t = int(msg.split(":", 1)[1])
                print(f"[ESP32] pong = {int(time.time() * 1000) - t} ms")
            elif msg == "transcrever":
                print("[ESP32] pediu a transcricao")
                disparar(transcrever_ultima())
            elif msg == "falar_ultimo":
                if ultima_transcricao:
                    disparar(falar(ultima_transcricao))
                else:
                    await enviar_esp32("erro:Nada para falar")
            elif msg.startswith("libras:"):
                if libras:
                    await enviar_libras("cmd:" + msg.split(":", 1)[1])
                else:
                    await enviar_esp32("erro:Camera ainda abrindo ou frase vazia")
            elif msg.startswith("modo:"):
                modo_atual = msg.split(":", 1)[1]
                print(f"[ESP32] modo: {modo_atual}")
                gerir_janela_libras(modo_atual == "libras")
                await enviar_libras("modo:" + modo_atual)
            else:
                print(f"[ESP32] {msg}")
    except websockets.ConnectionClosed:
        pass
    finally:
        clientes.discard(ws)
        libras.discard(ws)
        print("[-] cliente desconectado")


async def terminal():
    global eco
    loop = asyncio.get_running_loop()
    print("Comandos: <texto> | /gravar [s] | /transcrever | /eco | /ping | /vol 1.5 | /sair\n")
    while True:
        linha = (await loop.run_in_executor(None, sys.stdin.readline)).strip()
        if not linha:
            continue
        if linha == "/sair":
            return
        try:
            if linha == "/ping":
                await enviar_esp32(f"ping:{int(time.time() * 1000)}")
            elif linha.startswith("/vol"):
                await enviar_esp32(f"vol:{float(linha.split()[1])}")
            elif linha.startswith("/gravar"):
                partes = linha.split()
                seg = int(partes[1]) if len(partes) > 1 else 5
                await enviar_esp32(f"rec:{seg}")
            elif linha == "/transcrever":
                await transcrever_ultima(responder_esp32=False)
            elif linha == "/eco":
                eco = not eco
                print(f"Eco {'ligado' if eco else 'desligado'}.")
            else:
                await falar(linha)
        except Exception as e:
            print(f"Erro: {e!r}")


async def main():
    try:
        async with websockets.serve(tratador, HOST, PORT, max_size=None):
            print(f"Servidor WebSocket em ws://{HOST}:{PORT}")
            await terminal()
    finally:
        gerir_janela_libras(False)


if __name__ == "__main__":
    asyncio.run(main())