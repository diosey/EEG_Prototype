
# EEG Prototype — Wireless Brain Activity Acquisition System

A prototype electroencephalograph (EEG) for acquiring and recording the brain's electrical activity, with real-time visualization in both time and frequency domains.

Developed as a degree project at Universidad Politécnica Territorial de Aragua "Federico Brito Figueroa" (Venezuela).

---

## Overview

This project implements a complete EEG acquisition system composed of:

- **Analog front-end**: Low-noise biopotential acquisition circuit
- **Digital processing**: ESP32 microcontroller with FIR and IIR digital filters
- **Wireless communication**: WiFi for real-time data streaming
- **Web interface**: Browser-based visualization of EEG signals in time and frequency domains

The system captures brain electrical activity, applies digital filtering (31-tap FIR with Hamming window, 1st-order IIR, both with low-pass and high-pass configurations), and streams the processed data wirelessly to any device with a web browser.

---

## Specifications

| Parameter | Value |
|-----------|-------|
| Signal type | Brain electrical activity (EEG) |
| Channels | 1 |
| Sampling rate | 860 Hz |
| ADC resolution | 16 bits |
| Microcontroller | ESP32 |
| Digital filters | FIR (31-tap, Hamming) + IIR (1st order) |
| Filter types | Low-pass and high-pass |
| Frequency analysis | Power spectrum (FFT) |
| Communication | WiFi (802.11 b/g/n) |
| Visualization | Web browser (HTML/JavaScript) |
| Firmware language | C |
| Design tool | KiCad |

---

## Design Highlights

- **Complete system integration**: Analog acquisition + digital processing + wireless communication + web visualization in a single project.
- **Digital signal processing**: FIR and IIR filters implemented in JavaScript on the ESP32, with configurable low-pass and high-pass modes.
- **Frequency-domain analysis**: Real-time power spectrum computed via FFT.
- **Wireless data streaming**: WiFi WebSocket server streams EEG data to any browser.
- **Cross-platform visualization**: Works on PC, tablet, or smartphone without installing any application.
- **Open hardware design**: Full KiCad schematic, PCB, and Gerbers included.

---

## Repository Structure

- `hardware/` — KiCad schematic, PCB, Gerbers, BOM
- `firmware/` — ESP32 C source code (acquisition, filters, WiFi, DSP)
- `docs/` — Design documentation, calculations, thesis summary

---

## Signal Processing

### FIR Filter
- **Topology**: 31-tap FIR
- **Window**: Hamming
- **Configurations**: Low-pass and high-pass

### IIR Filter
- **Topology**: 1st-order IIR
- **Configurations**: Low-pass and high-pass

### Frequency Analysis
- FFT-based power spectrum
- Real-time display in web interface

---

## Web Interface

The web interface allows visualization of EEG signals from any device with a browser (PC, tablet, smartphone) connected to the same WiFi network.

Features:
- Real-time time-domain plot
- Real-time frequency-domain plot (power spectrum)
- Filter selection (FIR/IIR, LP/HP)

---

## Tools Used

- **KiCad** — Schematic capture, PCB layout
- **ESP-IDF / PlatformIO** — ESP32 firmware development
- **C** — Firmware programming language
- **HTML/CSS/JavaScript** — Web interface

---

## Team

- **David Obregon** 
- **Diosey Lugo** 

**Tutor:** Prof. Carlos Aponte

**Institution:** Universidad Politécnica Territorial de Aragua "Federico Brito Figueroa"
**Date:** April 2026

---

## Author Contact
- **Gmail:** dioseyadolfolugo@gmail.com
- **GitHub:** https://github.com/diosey
- **LinkedIn:** http://www.linkedin.com/in/adolfodiaz01
